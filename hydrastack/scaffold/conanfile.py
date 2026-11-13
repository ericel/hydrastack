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
        "app/*",
        "cmake/*",
        "ui/*",
        "public/*",
    )
    generators = ("CMakeDeps", "CMakeToolchain")

    default_options = {
        "drogon/*:shared": False,
        "trantor/*:shared": False,
        "openssl/*:shared": True,
        "c-ares/*:shared": True,
        "jsoncpp/*:shared": True,
        # Drogon does not use Cobalt; its optional SSL component is not
        # packaged consistently by the Boost 1.91 Conan recipe.
        "boost/*:without_cobalt": True,
    }

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

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
