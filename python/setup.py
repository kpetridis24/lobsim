from setuptools import find_packages, setup

setup(
    name="lobsim",
    version="0.1.0",
    description="LOBSIM Python bindings",
    packages=find_packages(exclude=("tests", "tests.*")),
    package_data={"lobsim": ["*.pyi", "_core*.so"]},
    entry_points={"console_scripts": ["lobsim=lobsim.cli:main"]},
)
