#pragma once

#include "common.hpp"

namespace rdmaio {

struct ReplyHeader {
  uint16_t reply_status;
  uint16_t reply_payload;
};

struct RequestHeader {
  uint16_t req_type;
  uint16_t req_payload;
};

typedef std::string Buf_t;

class Marshal {
 public:
  static Buf_t get_buffer(int size) {
    return std::string(size,'0');
  }

  static Buf_t forward(const Buf_t &b,int pos,int size) {
    return b.substr(pos,size);
  }

  template <typename T>
  static Buf_t serialize_to_buf(const T &t) {
    auto res = get_buffer(sizeof(T));
    memcpy((char *)(res.data()),&t,sizeof(T));
    return res;
  }

  template <typename T>
  static bool deserialize(const Buf_t &buf,T &t) {
    if (buf.size() != sizeof(T))
      return false;
    memcpy((char *)(&t),buf.data(),sizeof(T));
    return true;
  }

  static Buf_t null_req() {
    RequestHeader req = {.req_type = 0,.req_payload = 0};
    return serialize_to_buf(req);
  }

  static Buf_t null_reply() {
    ReplyHeader reply = {.reply_status = ERR,.reply_payload = 0};
    return serialize_to_buf(reply);
  }

};

} // end namespace rdmaio
