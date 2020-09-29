#!/usr/bin/env bash

# Replace to submodule
    rm -rf $@
    git rm -r $@
    read -p 'url > ' inputurl
    git submodule add ${inputurl} $@