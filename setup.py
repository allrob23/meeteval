from setuptools import Extension, setup
# for development
from Cython.Build import cythonize
ext_modules = cythonize(
    [
        'meeteval/wer/matching/cy_orc_matching.pyx',
        'meeteval/wer/matching/cy_mimo_matching.pyx',
     ]
)

setup(
    name="meeteval",
    python_requires=">=3.5",
    author="Thilo von Neumann",
    ext_modules=ext_modules,
    packages=["meeteval"],
)
