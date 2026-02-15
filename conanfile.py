from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class HydraStackConan(ConanFile):
    name = "hydrastack"
    version = "0.1.0"
    package_type = "library"
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }
    exports_sources = (
        "CMakeLists.txt",
        "README.md",
        "LICENSE",
        "engine/*",
        "cmake/*",
    )

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.get_safe("shared"):
            self.options.rm_safe("fPIC")

    def requirements(self):
        self.requires("drogon/1.9.10")
        self.requires("jsoncpp/1.9.5")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["HYDRA_BUILD_DEMO"] = False
        toolchain.variables["HYDRA_BUILD_UI"] = False
        toolchain.generate()

        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "HydraStack")
        self.cpp_info.set_property("cmake_target_name", "HydraStack::hydra_engine")
        self.cpp_info.libs = ["hydra_engine"]
