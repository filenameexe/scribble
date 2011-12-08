#!/bin/sh

for file in $(find . -name *.cpp)
do
    astyle -A3 --indent=spaces=4 --recursive *.cpp *.h
done
