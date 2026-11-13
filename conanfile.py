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
        "drogon/*:shared": False,
        "trantor/*:shared": False,
        "openssl/*:shared": True,
        "c-ares/*:shared": True,
        "jsoncpp/*:shared": True,
        # Drogon does not use Cobalt; its optional SSL component is not
        # packaged consistently by the Boost 1.91 Conan recipe.
        "boost/*:without_cobalt": True,
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
        # Keep the engine, scaffold, and consuming app on one native ABI.
        self.requires("drogon/1.9.14")
        self.requires("jsoncpp/1.9.8", force=True)
        self.requires("trantor/1.5.29", override=True)
        self.requires("openssl/3.5.9", override=True)
        self.requires("c-ares/1.34.9", override=True)
        self.requires("zlib/1.4", override=True)
        self.requires("boost/1.91.0", override=True)

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
