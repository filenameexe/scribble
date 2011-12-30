Build Process
-------------

    sudo apt-get install -y git-core vim astyle cmake nvidia-cg-toolkit build-essential ftgl-dev libqt4-dev libpulse-dev libglew1.5-dev libvisual-0.4-dev libsdl-dev

    cd ~
    mkdir repos
    cd repos
    git clone git://github.com/filenameexe/scribble.git
    git config color.ui true

    cd scribble/projectm/src
    cmake . -Wno-dev
    make
    sudo make install
