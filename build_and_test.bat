@echo off
setlocal

:: Setup VS environment
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

echo.
echo ===========================================
echo Building and Testing CUDA
echo ===========================================
set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2"
set "CUDACXX=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2\bin\nvcc.exe"
set "PATH=%CUDA_PATH%\bin;%PATH%"

if exist build_cuda rmdir /s /q build_cuda
mkdir build_cuda
cd build_cuda
cmake .. -DENABLE_CUDA=ON -DENABLE_SYCL=OFF -G "Ninja"
cmake --build . --config Release
if errorlevel 1 (
    echo CUDA Build Failed!
) else (
    echo CUDA Build Success!
    echo Running CUDA test...
    test_cuda_kernels.exe
)
cd ..

echo.
echo ===========================================
echo Building and Testing SYCL
echo ===========================================
call "C:\Program Files (x86)\Intel\oneAPI\setvars.bat"

if exist build_sycl rmdir /s /q build_sycl
mkdir build_sycl
cd build_sycl
:: Note: On Windows, 'icx' is the MSVC-compatible driver. 'icpx' expects Linux-style flags.
cmake .. -DENABLE_CUDA=OFF -DENABLE_SYCL=ON -G "Ninja" -DCMAKE_CXX_COMPILER=icx
cmake --build . --config Release
if errorlevel 1 (
    echo SYCL Build Failed!
) else (
    echo SYCL Build Success!
    echo Running SYCL test...
    test_sycl_kernels.exe
)
cd ..
