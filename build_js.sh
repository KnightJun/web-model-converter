em++  -DONNX_ML=1 -DONNX_NAMESPACE=onnx -DNDEBUG -O2 -s ALLOW_MEMORY_GROWTH=1 -s EXPORTED_FUNCTIONS=\[_onnx2ncnn_export,_create_exporter,_get_buffer1,_get_buffer2,_get_buffer_size1,_get_buffer_size2\] -s EXTRA_EXPORTED_RUNTIME_METHODS=\[ccall,cwrap\] @cpp/build/CMakeFiles/export.dir/includes_CXX.rsp -std=gnu++11 cpp/export.cpp -o cpp/build/export.js -L cpp/build/ -L cpp/build/third_party/onnx/ -L cpp/build/third_party/protobuf/cmake/ -lonnx2ncnn -lcaffe2ncnn -lonnx -lonnx_proto -lprotobuf

