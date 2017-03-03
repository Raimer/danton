#!/bin/bash

# Set the environment for the submodules.
. ent/setup.sh
. pumas/setup.sh
. tauola-c/setup.sh

# Script base directory.
basedir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Set the materials.
export PUMAS_MDF=$basedir/materials/materials.xml
export PUMAS_DEDX=$basedir/materials/dedx
