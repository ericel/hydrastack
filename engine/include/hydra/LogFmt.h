#pragma once

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace hydra::logfmt {

inline const char *onOff(bool value) {
    return value ? "on" : "off";
}

class Line {
  public:
    explicit Line(std::string prefix) : prefix_(std::move(prefix)) {}

    Line &block(std::string text) {
        parts_.push_back(std::move(text));
        return *this;
    }

    Line &group(
        const std::string &name,
        std::initializer_list<std::pair<std::string, std::string>> fields) {
        std::ostringstream oss;
        oss << name << "{";
        bool first = true;
        for (const auto &field : fields) {
            if (!first) {
                oss << ", ";
            }
            first = false;
            oss << field.first << "=" << field.second;
        }
        oss << "}";
        parts_.push_back(oss.str());
        return *this;
    }

    [[nodiscard]] std::string str() const {
        std::ostringstream oss;
        oss << prefix_;
        for (const auto &part : parts_) {
            oss << " | " << part;
        }
        return oss.str();
    }

  private:
    std::string prefix_;
    std::vector<std::string> parts_;
};

}  // namespace hydra::logfmt
