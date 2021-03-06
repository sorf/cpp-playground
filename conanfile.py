from conans import ConanFile, CMake, tools

class CppPlaygroundConan(ConanFile):
    name = "cpp_playground"
    version = "0.1"
    license = "The Unlicense"
    url = "https://github.com/sorf/cpp-playground"
    description = "C++ playground project"
    settings = "os", "arch", "compiler", "build_type"
    options = {}
    generators = "cmake"
    exports_sources = "source/*", ".clang-format", ".clang-tidy", "CMakeLists.txt", "format_check.sh"
    requires = "boost/1.69.0@conan/stable"
    default_options = "boost:header_only=True"

    def build(self):
        cmake = CMake(self)
        cmake.verbose = True
        if self.settings.compiler == "Visual Studio":
            cmake.definitions["CONAN_CXX_FLAGS"] += " /W4 /WX /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS" +\
                                                    " /experimental:external /external:anglebrackets /external:W0" +\
                                                    " /D_WIN32_WINNT=0x0A00 /DWIN32_LEAN_AND_MEAN" +\
                                                    " /DBOOST_ALL_NO_LIB=1" +\
                                                    " /DBOOST_ASIO_STANDALONE /DBOOST_ASIO_NO_EXTENSIONS"
        elif self.settings.compiler == "gcc":
            cmake.definitions["CONAN_CXX_FLAGS"] += " -Wall -Wextra -Werror" +\
                                                    " -DBOOST_ASIO_STANDALONE -DBOOST_ASIO_NO_EXTENSIONS"
        elif self.settings.compiler == "clang":
            cmake.definitions["CONAN_CXX_FLAGS"] += " -Wall -Wextra -Werror -Wglobal-constructors" +\
                                                    " -DBOOST_ASIO_STANDALONE -DBOOST_ASIO_NO_EXTENSIONS"

        if not self.settings.os == "Windows":
            cmake.definitions["CONAN_CXX_FLAGS"] += " -pthread"

        if self.settings.compiler == "clang":
            clang_tidy = tools.get_env("CLANG_TIDY")
            if clang_tidy:
                path_clang_tidy = tools.which(clang_tidy)
                if path_clang_tidy:
                    cmake.definitions["CLANG_TIDY_COMMAND"] = path_clang_tidy

        cmake.configure()
        cmake.build()

