#!/usr/bin/env python

from distutils.core import setup
from catkin_pkg.python_setup import generate_distutils_setup

d = generate_distutils_setup(
    packages=['dcsl_low_level_control'],
    package_dir={'': 'src'}
)

setup(**d)
