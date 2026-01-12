from setuptools import find_packages, setup

setup(
    name="simex",
    version="0.1.0",
    description="SIMEX Python bindings",
    packages=find_packages(exclude=("tests", "tests.*")),
    package_data={"simex": ["*.pyi", "_core*.so"]},
)
