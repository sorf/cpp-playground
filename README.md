# cpp-playground

C++ testing playground.


## Building

### Linux (clang and gcc)
conan install . --install-folder=tmp/build-ct -pr profiles/linux-64-clang-7-libstdc++-release-tidy <br>
conan build . --install-folder=tmp/build-ct --build-folder=tmp/build-ct

conan install . --install-folder=tmp/build-gd -pr profiles/linux-64-gcc-8-libstdc++11-debug <br>
conan build . --install-folder=tmp/build-gd --build-folder=tmp/build-gd

### Windows (MSVC)
conan install . --install-folder=tmp\build-vsd -pr profiles\windows-64-vs15-release <br>
conan build . --install-folder=tmp\build-vsr --build-folder=tmp\build-vsr

conan install . --install-folder=tmp\build-vsd -pr profiles\windows-64-vs15-debug <br>
conan build . --install-folder=tmp\build-vsd --build-folder=tmp\build-vsd
