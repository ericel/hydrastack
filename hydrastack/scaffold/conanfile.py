from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout


class HydraStackConan(ConanFile):
    name = "hydrastack"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = (
        "CMakeLists.txt",
        "README.md",
        "LICENSE",
        "engine/*",
        "demo/*",
        "cmake/*",
        "ui/*",
        "public/*",
    )
    generators = ("CMakeDeps", "CMakeToolchain")

    def requirements(self):
        self.requires("drogon/1.9.10")
        self.requires("jsoncpp/1.9.5")

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
