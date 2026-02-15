#pragma once

namespace hydra {

class V8Platform {
  public:
    static void initialize();
    static void shutdown();
};

}  // namespace hydra
