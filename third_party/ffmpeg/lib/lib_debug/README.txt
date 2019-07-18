Built with:

$ ./configure --target-os=win64 --arch=x86_64 --toolchain=msvc --extra-cflags=" -EHa -nologo -D 'DEBUG' -D 'WINDOWS' -D 'HAVE_STRUCT_TIMESPEC' -D 'API_EXT_PARAM_LIST'" --enable-debug --disable-stripping --disable-optimizations --disable-cuda --disable-cuvid --disable-d3d11va --disable-dxva2 --disable-nvenc --disable-vaapi --disable-vdpau --enable-libx264 --enable-gpl
$ make
$ make install