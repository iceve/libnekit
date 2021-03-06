#!/usr/bin/env bash

CURRENTPATH=`pwd`

pushd `dirname $0` > /dev/null
SCRIPTPATH=`pwd`
popd > /dev/null

LIBUV_PATH=$SCRIPTPATH/../deps/libuv/
cd $LIBUV_PATH

git clone https://chromium.googlesource.com/external/gyp.git build/gyp

mkdir -p ../deps_build/lib
mkdir -p ../deps_build/include
cp -r include/* ../deps_build/include/

if [ "$(uname)" == "Darwin" ]; then
    ./gyp_uv.py -f xcode -R libuv
    if [ "$ARCH" == "x32" ]; then
        xcodebuild ARCHS=i386 ONLY_ACTIVE_ARCHS=NO -project uv.xcodeproj \
                   -configuration Release -target libuv
    else
        xcodebuild ARCHS=x86_64 ONLY_ACTIVE_ARCHS=NO -project uv.xcodeproj \
                   -configuration Release -target libuv
    fi
    pwd
    cp build/Release/libuv.a ../deps_build/lib/
elif [ "$(expr substr $(uname -s) 1 5)" == "Linux" ]; then
    if [ "$ARCH" == "x32" ]; then
        ./gyp_uv.py -f make -Dtarget_arch=x32 -R libuv
    elif [ "$ARCH" == "ia32" ]; then
        ./gyp_uv.py -f make -Dtarget_arch=ia32 -R libuv
    else
        ./gyp_uv.py -f make -R libuv
    fi
    BUILDTYPE=Release make -C out
    cp out/Release/libuv.a ../deps_build/lib/
fi

cd $CURRENTPATH

