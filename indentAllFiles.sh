#!/bin/sh

for file in $(find . -name *.cpp)
do
    indent --no-tabs --line-length 120 --k-and-r-style $file
done
