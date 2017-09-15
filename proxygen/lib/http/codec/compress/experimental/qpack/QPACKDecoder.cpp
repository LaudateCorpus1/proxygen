/*
 *  Copyright (c) 2017, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/compress/experimental/qpack/QPACKDecoder.h>
// for hpack2headerCodecError
#include <proxygen/lib/http/codec/compress/HPACKDecoder.h>

#include <algorithm>
#include <folly/Memory.h>
#include <proxygen/lib/http/codec/compress/HeaderCodec.h>
#include <proxygen/lib/http/codec/compress/Huffman.h>

using folly::IOBuf;
using folly::io::Cursor;
using std::list;
using std::string;
using std::unique_ptr;
using std::vector;
using proxygen::HPACK::DecodeError;

namespace {
const std::chrono::seconds kDecodeTimeout{5};
}

namespace proxygen {

const huffman::HuffTree& QPACKDecoder::getHuffmanTree() const {
  return huffman::huffTree();
}

QPACKDecoder::~QPACKDecoder() {
  while (!decodeRequests_.empty()) {
    auto dreq = decodeRequests_.begin();
    dreq->err = HPACK::DecodeError::CANCELLED;
    checkComplete(dreq);
  }
  // futures are left running but have DestructorCheck's
}

bool QPACKDecoder::decodeStreaming(
    Cursor& cursor,
    uint32_t totalBytes,
    HeaderCodec::StreamingCallback* streamingCb) {

  decodeRequests_.emplace_front(streamingCb, totalBytes);
  auto dreq = decodeRequests_.begin();
  HPACKDecodeBuffer dbuf(getHuffmanTree(), cursor, totalBytes,
                         maxUncompressed_);
  while (!dreq->hasError() && !dbuf.empty()) {
    dreq->pending++;
    decodeHeader(dbuf, dreq);
  }
  dreq->allSubmitted = !dreq->hasError();
  dreq->consumedBytes = dbuf.consumedBytes();
  // checkComplete also handles errors
  auto done = checkComplete(dreq);
  queuedBytes_ = pendingDecodeBytes_;
  for (const auto& dr: decodeRequests_) {
    queuedBytes_ += dr.decodedSize.uncompressed;
  }
  return done;
}

void QPACKDecoder::decodeHeader(HPACKDecodeBuffer& dbuf,
                                DecodeRequestHandle dreq) {
  uint8_t byte = dbuf.peek();
  if (byte & HPACK::HeaderEncoding::INDEXED) {
    decodeIndexedHeader(dbuf, dreq);
  } else {
    // LITERAL_NO_INDEXING or LITERAL_INCR_INDEXING
    decodeLiteralHeader(dbuf, dreq);
  }
}

void QPACKDecoder::decodeIndexedHeader(HPACKDecodeBuffer& dbuf,
                                       DecodeRequestHandle dreq) {
  uint32_t index;
  dreq->err = dbuf.decodeInteger(7, index);
  if (dreq->hasError()) {
    LOG(ERROR) << "Decode error decoding index err=" << dreq->err;
    return;
  }
  // validate the index
  if (index == 0 || !isValid(index)) {
    LOG(ERROR) << "received invalid index: " << index;
    dreq->err = HPACK::DecodeError::INVALID_INDEX;
    return;
  }
  if (isStatic(index)) {
    emit(dreq, getStaticHeader(index));
  } else {
    getDynamicHeader(index)
      .then([this, dreq] (QPACKHeaderTable::DecodeResult res) {
          // TODO: memory
          emit(dreq, res.ref);
        })
      .onTimeout(kDecodeTimeout, [this, dreq] {
          dreq->err = HPACK::DecodeError::TIMEOUT;
          checkComplete(dreq);
        })
      .onError([] (folly::BrokenPromise&) {
          // means the header table is being deleted
          VLOG(4) << "Broken promise";
        })
      .onError([this, dreq] (const std::runtime_error&) {
          dreq->err = HPACK::DecodeError::INVALID_INDEX;
          checkComplete(dreq);
        });
  }
}

bool QPACKDecoder::isValid(uint32_t index) {
  if (!isStatic(index)) {
    // all dynamic indexes must be considered valid, since they might come out
    // of order
    return true;
  }
  return getStaticTable().isValid(globalToStaticIndex(index));
}


void QPACKDecoder::emit(DecodeRequestHandle dreq, const HPACKHeader& header) {
  // would be nice to std::move here
  dreq->cb->onHeader(header.name, header.value);
  dreq->decodedSize.uncompressed += header.bytes();
  dreq->pending--;
  checkComplete(dreq);
}


bool QPACKDecoder::checkComplete(DecodeRequestHandle dreq) {
  if (dreq->pending == 0 && dreq->allSubmitted) {
    dreq->cb->onHeadersComplete(dreq->decodedSize);
    decodeRequests_.erase(dreq);
    return true;
  } else if (dreq->hasError()) {
    dreq->cb->onDecodeError(hpack2headerCodecError(dreq->err));
    decodeRequests_.erase(dreq);
    return true;
  }
  return false;
}

void QPACKDecoder::decodeLiteralHeader(HPACKDecodeBuffer& dbuf,
                                       DecodeRequestHandle dreq) {
  uint8_t byte = dbuf.peek();
  bool indexing = byte & HPACK::HeaderEncoding::LITERAL_INCR_INDEXING;
  uint8_t indexMask;
  uint8_t length = 6;
  uint32_t newIndex = 0;
  if (indexing) {
    dreq->err = dbuf.decodeInteger(length, newIndex);
    if (dreq->hasError()) {
      LOG(ERROR) << "Decode error decoding newIndex err=" << dreq->err;
      return;
    }
    if (isStatic(newIndex)) {
      LOG(ERROR) << "Decode error newIndex=" << newIndex;
      dreq->err = HPACK::DecodeError::INVALID_INDEX;
      return;
    }
    newIndex = globalToDynamicIndex(newIndex);
    if (dbuf.empty()) {
      LOG(ERROR) << "Decode error underflow";
      dreq->err = HPACK::DecodeError::BUFFER_UNDERFLOW;
      return;
    }

    byte = dbuf.peek();
    length = 8;
    indexMask = 0xFF; // 1111 1111
  } else {
    // HPACK::TABLE_SIZE_UPDATE is QPACK::DELETE
    bool deleteOp = byte & HPACK::HeaderEncoding::TABLE_SIZE_UPDATE;
    if (deleteOp) {
      decodeDelete(dbuf, dreq);
      return;
    } else {
      //bool neverIndex = byte & HPACK::HeaderEncoding::LITERAL_NEVER_INDEXING;
      // TODO: we need to emit this flag with the headers
    }
    indexMask = 0x0F; // 0000 1111
    length = 4;
  }
  QPACKHeaderTable::DecodeFuture nameFuture{
    QPACKHeaderTable::DecodeResult(HPACKHeader())};
  uint32_t nameIndex;
  bool nameIndexed = byte & indexMask;
  if (nameIndexed) {
    dreq->err = dbuf.decodeInteger(length, nameIndex);
    if (dreq->hasError()) {
      LOG(ERROR) << "Decode error decoding index err=" << dreq->err;
      return;
    }
    // validate the index
    if (!isValid(nameIndex)) {
      LOG(ERROR) << "received invalid index: " << nameIndex;
      dreq->err = HPACK::DecodeError::INVALID_INDEX;
      return;
    }
    nameFuture = getHeader(nameIndex);
  } else {
    // skip current byte
    dbuf.next();
    HPACKHeader header;
    dreq->err = dbuf.decodeLiteral(header.name);
    if (dreq->hasError()) {
      LOG(ERROR) << "Error decoding header name err=" << dreq->err;
      return;
    }
    // This path might be a little inefficient in the name of symmetry below
    nameFuture = folly::makeFuture<QPACKHeaderTable::DecodeResult>(
      QPACKHeaderTable::DecodeResult(std::move(header)));
  }
  // value
  std::string value;
  dreq->err = dbuf.decodeLiteral(value);
  if (dreq->hasError()) {
    LOG(ERROR) << "Error decoding header value name=" <<
      ((nameFuture.isReady()) ?
       nameFuture.value().ref.name :
       folly::to<string>("pending=", nameIndex)) <<
      " err=" << dreq->err;
    return;
  }

  pendingDecodeBytes_ += value.length();
  nameFuture
    .then(
      [this, value=std::move(value), dreq, indexing, newIndex]
      (QPACKHeaderTable::DecodeResult res) mutable {
        pendingDecodeBytes_ -= value.length();
        HPACKHeader header;
        if (res.which == 1) {
          header.name = std::move(res.value.name);
        } else {
          header.name = res.ref.name;
        }
        header.value = std::move(value);
        // get the memory story straight
        emit(dreq, header);
        if (indexing) {
          table_.add(header, newIndex);
        }
      })
    .onTimeout(kDecodeTimeout, [this, dreq] {
        dreq->err = HPACK::DecodeError::TIMEOUT;
        checkComplete(dreq);
      })
    .onError([] (folly::BrokenPromise&) {
        // means the header table is being deleted
        VLOG(4) << "Broken promise";
      })
    .onError([this, dreq] (const std::runtime_error&) {
        dreq->err = HPACK::DecodeError::INVALID_INDEX;
        checkComplete(dreq);
      });
}

void QPACKDecoder::decodeDelete(HPACKDecodeBuffer& dbuf,
                                DecodeRequestHandle dreq) {
  uint32_t refcount;
  uint32_t delIndex;
  dreq->err = dbuf.decodeInteger(5, refcount);
  if (dreq->hasError() || refcount == 0) {
    LOG(ERROR) << "Invalid recount decoding delete refcount=" << refcount;
    return;
  }
  dreq->err = dbuf.decodeInteger(8, delIndex);
  if (dreq->hasError() || delIndex == 0 || isStatic(delIndex)) {
    LOG(ERROR) << "Invalid index decoding delete delIndex=" << delIndex;
    return;
  }

  // no need to hold this request
  dreq->pending--;
  table_.decoderRemove(globalToDynamicIndex(delIndex), refcount)
    .then([this, delIndex] {
        VLOG(4) << "delete complete for delIndex=" << delIndex;
        callback_.ack(globalToDynamicIndex(delIndex) - 1);
      })
    .onTimeout(kDecodeTimeout, [this, delIndex] {
        LOG(ERROR) << "Timeout trying to delete delIndex=" << delIndex;
        callback_.onError();
      })
    .onError([this, delIndex] (const std::runtime_error&) {
        LOG(ERROR) << "Decode error trying to delete delIndex=" << delIndex;
        callback_.onError();
      });

}
}
