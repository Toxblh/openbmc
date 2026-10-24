#pragma once
// stand-in for obmc-ikvm's Server/Video: what ikvm_input.cpp uses of them
#include "ikvm_input.hpp"
#include <cstddef>
namespace ikvm {
class Video { public: size_t getWidth() const { return 1920; } size_t getHeight() const { return 1080; } };
class Server {
  public:
    struct ClientData { Input* input; };
    const Video& getVideo() const { return video; }
    Video video;
};
}
