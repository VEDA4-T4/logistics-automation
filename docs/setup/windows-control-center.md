# Windows Qt 관제 시스템 설치

## 요구 사항

- Windows 10/11 64-bit
- Qt 6.10 이상: `Widgets`, `Network`, `Multimedia`, `Mqtt`
- Qt 버전과 일치하는 MinGW 64-bit kit
- CMake 3.20 이상과 Ninja
- Visual Studio 2022에 포함된 vcpkg 또는 별도 vcpkg

Qt Creator에서는 `control-center/CMakeLists.txt`를 직접 여는 구성이 가장 단순합니다.

## vcpkg 의존성

PowerShell에서 저장소 루트로 이동한 뒤 manifest 의존성을 설치합니다.

```powershell
cd C:\programming\workspace\logistics-automation

$VcpkgExe = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg\vcpkg.exe"
& $VcpkgExe install --triplet x64-windows
```

반드시 `vcpkg.json`이 있는 저장소 루트에서 실행해야 합니다. 성공하면
`vcpkg_installed/x64-windows/include/nlohmann/json.hpp`가 생성됩니다.

`this vcpkg instance requires a manifest with a specified baseline` 오류가 발생하면 최신 `main`의
`vcpkg.json`을 사용 중인지 확인합니다. 이 저장소의 manifest에는 `builtin-baseline`이 포함되어 있습니다.

## Qt MQTT 모듈

Qt Maintenance Tool에서 Qt MQTT를 설치할 수 있으면 해당 방법을 우선 사용합니다. 사용하는 Qt 버전에 MQTT 모듈이
없다면 같은 버전·같은 MinGW kit로 직접 빌드합니다. 아래 예시는 Qt 6.11.1입니다.

```powershell
$QtRoot = "C:/Qt/6.11.1/mingw_64"
$WorkDir = Join-Path $env:TEMP "qtmqtt-6.11.1"
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\CMake_64\bin;$QtRoot/bin;$env:PATH"

git clone --branch v6.11.1 --depth 1 https://github.com/qt/qtmqtt.git "$WorkDir/src"
New-Item -ItemType Directory -Path "$WorkDir/build" -Force | Out-Null

Push-Location "$WorkDir/build"
& "$QtRoot/bin/qt-configure-module.bat" "$WorkDir/src" -cmake-generator Ninja -- `
  "-DCMAKE_BUILD_TYPE=Release" `
  "-DCMAKE_INSTALL_PREFIX:PATH=$QtRoot" `
  "-DQT_BUILD_TESTS=OFF" `
  "-DQT_BUILD_EXAMPLES=OFF"
Pop-Location

cmake --build "$WorkDir/build" --parallel
cmake --install "$WorkDir/build"
Test-Path "$QtRoot/lib/cmake/Qt6Mqtt/Qt6MqttConfig.cmake"
```

마지막 명령은 `True`여야 합니다. 설치 권한 오류가 발생하면 관리자 PowerShell에서 `cmake --install`만 다시
실행합니다.

## 빌드

Qt Creator에서 `Desktop Qt 6.x MinGW 64-bit` kit를 선택하고 CMake를 다시 구성합니다. 이전 실패가 캐시되어 있으면
`Build > Clear CMake Configuration` 후 다시 실행합니다.

명령줄에서는 다음과 같이 빌드할 수 있습니다.

```powershell
$QtCMake = "C:\Qt\Tools\CMake_64\bin\cmake.exe"
$BuildDir = "build-control-center"

& $QtCMake -S control-center -B $BuildDir -G Ninja
& $QtCMake --build $BuildDir --parallel 4
& $QtCMake --build $BuildDir --target test
```

실행 전에 `control-center/config/control-centor.ini.example`을 복사해 로컬 설정을 만듭니다. 자세한 내용은
[런타임 설정](../guides/runtime-configuration.md)을 참고하세요.

## 배포

Qt Creator 밖에서 실행하거나 다른 PC에 복사할 때 Qt DLL을 함께 배포합니다.

```powershell
& "$QtRoot/bin/windeployqt.exe" --release path\to\logistics_control_center.exe
```

Qt MQTT 모듈의 배포 라이선스 조건도 사용 중인 Qt 라이선스와 함께 확인해야 합니다.
