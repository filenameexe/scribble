Some subset of this made things work. This needs to be tidied up.

sudo apt-get install libglew1.5 libglew1.5-dev ftgl-dev libpulse-dev subversion cmake libvisual-0.4-dev libsdl-dev libqt4-dev build-essential

<!-- svn co https://projectm.svn.sf.net/svnroot/projectm/trunk projectM-Trunk -->
svn co https://projectm.svn.sf.net/svnroot/projectm/tags/projectM-2.0.0 projectM-2.0.0

vi src/projectM-pulseaudio/qprojectM-pulseaudio.cpp
    :46
    i
    #include <sys/types.h>
    #include <sys/stat.h>

cd $HOME/src
cmake .
ccmake .
    'c' - Configure
    'g' - Generate
make
sudo make install

projectM-pulseaudio


sudo apt-get install libxmu-dev libxmu6 libxi-dev libxmu-dev
sudo apt-get install libglew.* ftgl-dev qt4-dev-tools libsdl-dev^C
sudo aptitude install nvidia-cg-toolkit

Add
http://nvidia-texture-tools.googlecode.com/svn-history/r96/trunk/cmake/FindGLEW.cmake
to src/projectM-pulseaudio/FindGLEW.cmake

This was also done. Dunno??
Glew Makefile:
    GLEW_DEST ?= /usr
    GLEW_DEST ?= /usr/local

On rev. 1364
