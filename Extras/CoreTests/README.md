# CoreTests — Tests und Python-Anbindung für HybridAcousticsCore

Baut den Kern aus `../../Source/HybridAcousticsCore` (Header direkt in `Public/`, Quellen direkt in `Private/`) außerhalb von Unreal:
- `hac_tests`: GoogleTest-Suite (Moden, T60, Bake, Hybrid, Faltung, Gitter)
- `hybridcore`: Python-Modul für die Evaluations-Notebooks
- `python/evaluate.py`: Moden- und T60-Auswertung → CSV + PNG

Dieser Ordner liegt bewusst **nicht** unter `Source/`: UBT kompiliert jede `.cpp` im Modulordner mit.

## Einmalig: Werkzeuge

- Visual Studio 2022 mit Workload „Desktopentwicklung mit C++“ (enthält CMake und Ninja)
- Git (für den automatischen Download von GoogleTest)
- Python 3 mit `pip install pybind11 numpy matplotlib`

## Bauen

In der **„x64 Native Tools Command Prompt for VS 2022“**:

```bat
cd "C:\Users\laura\Documents\Unreal Projects\HybridRoomTest\Plugins\HybridAcoustics\Extras\CoreTests"
python -m pybind11 --cmakedir
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -Dpybind11_DIR="<Ausgabe der Zeile davor>"
cmake --build build
```

Ohne Python-Modul: `-DHAC_BUILD_PYTHON=OFF`. Mit vcpkg-GoogleTest zusätzlich
`-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake`.

## Tests starten

```bat
build\hac_tests.exe
build\hac_tests.exe --gtest_filter=Modes.*
ctest --test-dir build
```

## Auswertung

```bat
python python\evaluate.py
```

Einstellungen im CONFIG-Block oben in `evaluate.py`; Ergebnisse in `python\results\`.
Python-Version beim Bauen und beim Ausführen muss dieselbe sein (das Modul heißt z. B.
`hybridcore.cp312-win_amd64.pyd`).

## Neue Dateien im Kern

Alle `.cpp` aus `Private/` werden automatisch eingesammelt (außer `HybridAcousticsCoreModule.cpp`).
Eine neue Datei wird beim nächsten `cmake --build build` erkannt, die `CMakeLists.txt` muss nicht
angepasst werden.
