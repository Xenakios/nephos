cmake -DCMAKE_C_COMPILER=clang-20 \
      -DCMAKE_CXX_COMPILER=clang++-20 \
      -DCMAKE_CXX_FLAGS="-stdlib=libstdc++" \
      -DCMAKE_C_FLAGS="-stdlib=libstdc++" \
      -DCMAKE_BUILD_TYPE=Release
      ..

cmake --build . --target NephosCLI
