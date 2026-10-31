# HybridAcoustics

Hybride Raumakustik für Unreal Engine 5: FDTD (Wellengleichung, 2. oder 4. Ordnung) unterhalb der
Schroeder-Frequenz, Steam-Audio-Raytracing darüber, binaurale Wiedergabe (HRTF) mit dekorreliertem Nachhall.
Entstanden im Rahmen meiner Masterarbeit.

## Voraussetzungen
- Unreal Engine 5.8, Windows, Visual Studio 2022+ mit „Desktopentwicklung mit C++“
- Steam Audio SDK 4.8.1 (siehe Source/ThirdParty/SteamAudio/README.md)

## Installation
1. Repository nach `<Projekt>/Plugins/HybridAcoustics` klonen
2. Steam Audio SDK ablegen
3. Projektdateien neu erzeugen, Projekt bauen
4. Projekteinstellungen → Plattformen → Windows → Audio: Spatialization Plugin = „Hybrid Acoustics“

## Verwendung
`HybridAcousticsRoom` ins Level ziehen, Box auf den Raum skalieren, Quelle zuweisen, „Bake Now“.

## Tests und Auswertung (ohne Unreal)
Siehe `Extras/CoreTests` (CMake, GoogleTest, Python-Modul `hybridcore`).
