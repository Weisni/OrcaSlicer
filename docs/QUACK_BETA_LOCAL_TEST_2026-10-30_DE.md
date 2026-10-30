# Lokale Quack-Testversion auf OrcaSlicer 2.5.0 Beta

Stand: 30.10.2026. Version: **2.5.25-beta.local**, Windows x64 Release.

## Start und Daten

Im Integrations-Arbeitsbaum liegt der Starter unter
`build-test/QuackSlicer-Beta-Test/Start-Quack-Beta-Test.cmd`.
Er startet `runtime/quack-slicer.exe` mit `build-test/test-profile` als getrenntem
Datenverzeichnis. Direktes Starten der EXE verwendet ebenfalls einen eigenen
App-Schlüssel; für die vorbereitete Datenkopie bitte den Starter verwenden.

Die Testkopie enthält gespeicherte Benutzerprofile und einen konsistenten
SQLite-Backup-Snapshot der Materialverwaltung. Zugangsdaten, Netzwerk-Plugin,
Druckerverbindungen und produktive HA-Konfiguration wurden nicht übernommen.
HA-Anbindung, Vorheizen und Profil-Synchronisation sind zunächst deaktiviert.
Abhängige Benutzerprofile können ohne ihren ursprünglichen Elternpreset oder
HA-Kontext unvollständig erscheinen; für Vergleichsläufe liegen separat
aufgelöste offizielle P2S-/PLA-Einstellungen unter `build-test/comparison/settings`.
Die Kopie ist zum Testen bestimmt; Änderungen darin werden nicht zurückgeschrieben.
Der Standard-3MF-Farb-/Gruppierungsdialog wurde mit dem synthetischen Zweiermodell
geöffnet, den vorhandenen Slots zugeordnet und beide Objekte auf der Platte angezeigt.

Die laufende Materialverwaltung hat 30 aktive Rollen, 10,93 kg verfügbares Material
und 8,1 g Reservierungen aus der Kopie angezeigt. Das bestätigt Öffnen/Lesen dieser
Datenkopie. Die ursprüngliche Datenbank bleibt maßgeblich. Nach dem GUI-Test stimmen alle
11 Tabellenzeilenzahlen (insgesamt 1.422 Zeilen) zwischen Quelle und Kopie überein;
beide bestehen die Integritätsprüfung, die produktive Konfigurationsdatei ist unverändert.

## Integrierter Stand

- Quack-Basis: `2e38319f4e0762bc4cf44d46a28160e0fa2ae3f5` (2.5.24).
- Vollständiger Orca-Beta-Tag: `345efec64c7b95a05ce5cf96df0dde66a158835c`.
- Separater Zweig: `codex/integrate-orca-2.5.0-beta`.
- CAD aktiviert; frische passende wxWidgets-, Python-3.12.13-, CGAL-6.2.1-,
  OCCT-8.0.1- und weitere Laufzeit-/Build-Abhängigkeiten.
- Material-/Auftragsverwaltung und HA-Verträge, objektbezogene Towers,
  Projected-/Normalentiefen-Färbung, Wave Overhangs und organische Kontaktkorrekturen erhalten.
- Die normalen oberen/unteren Farbtiefen nutzen die Beta-Standardoptionen.
- Eigene Zugangsdaten-Dienste für Quack und Testbuild; keine Nutzung von Orcas
  Cloud-Token. Plugins erhalten keinen direkten Zugriff auf `filament_inventory`
  einschließlich SQLite-WAL/SHM und darunterliegenden Sicherungen.
- Testbuild registriert keine Dateizuordnungen, installiert keine Produktionsupdates
  und verwendet eine eigene Windows-Anwendungsidentität.

83 Konfliktdateien wurden aufgelöst. Die 79 Dateien mit Textkonflikten sind im
[Auflösungsprotokoll](upstream/orca-2.5.0-beta-merge-resolutions.jsonl) dokumentiert.
Vier Modify/Delete-Fälle wurden separat erhalten: ObjColorDialog für Quacks
Standard-3MF-Farbzuordnung; alte wxMediaCtrl2-Dateien bleiben ungenutzt, während der
Build die neue Beta-Medienimplementierung mit Quacks Playback-Anforderung verwendet.

## Lokale Prüfergebnisse

| Prüfung | Ergebnis |
| --- | --- |
| Frischer Windows-Abhängigkeits- und Anwendungsbuild | erfolgreich, CAD ON |
| Native CTest-Sammlung | 3.044 registriert, 0 fehlgeschlagen, 9 übersprungen |
| Übersprungene Fälle | 5 NumPy-Ansichten ohne NumPy im Testinterpreter; 4 symbolische Links ohne passende Windows-Rechte |
| HA Python mit gebündeltem Python 3.12.13 | 436 Tests erfolgreich |
| HA JavaScript-Oberflächenprüfungen | 11 Testdateien erfolgreich |
| Alle gettext-Kataloge | Format-/Headerprüfung und MO-Kompilierung erfolgreich; Warnungen zu unvollständigen Übersetzer-Headern |
| Kopierte SQLite-Datenbank | `integrity_check=ok`, Oberfläche öffnet Bestand/Reservierungen |
| Review-Befunde | 2 wichtige Befunde durch neue RED→GREEN-Tests behoben; keine belastbaren Minor-Befunde |

Die Regressionen prüfen u.a. gemischte virtuelle Filamente gegen physische
Verbrauchsquellen, Wave-Ausgabe ein/aus und Fahrgeschwindigkeit bei Achsumrechnung,
organische Kontakte, objektweise Towers, Preset-/HA-Änderungen, Importe und
Plugin-Laufzeit. Beim ersten vollständigen Lauf erkannte die Suite drei
Slicing-Regressionsfälle; diese wurden vor dem abschließenden grünen Lauf behoben.

Mit identischen aufgelösten offiziellen P2S-0,4-mm-/Generic-PLA-/0,20-mm-Einstellungen
wurde `paint-depth-coupon.stl` in offizieller Beta und Quack gesliced:
**4.009 identische G0/G1/G2/G3-Bewegungszeilen**, identischer errechneter Verbrauch
805,31 mm / 1,94 cm³ / 2,40 g. Vollständige Dateien unterscheiden sich durch
Versions-/Konfigurationsmetadaten. Ergebnis: `build-test/comparison/cli-baseline-result.json`.
Das ist ein einzelner Grundfall mit deaktivierten Zusatzoptionen, kein Nachweis
vollständiger Gleichwertigkeit oder gleicher Druckqualität/Leistung.

Die offizielle unveränderte Beta liegt separat unter `build-test/stock-orca/runtime`.
Archiv-SHA256: `e11df6023a91ae771399f9beaeb333473a2c4e4f50a0126ae0a2e25b88d8d0c2`.
Der kleine Console-Probe-Wrapper ruft nur deren unveränderten exportierten Einstieg
auf; er ändert weder DLL noch Slicingverhalten.

## Orca-Standardfunktionen vergleichen

Die [deutsche Vergleichsanleitung](ORCA_STANDARD_COMPARISON_TRIALS_DE.md) und vier
synthetische Dateien unter `build-test/comparison` sind vorbereitet. Die
[Matrix](upstream/quack-orca-standard-comparison.md) trennt vollständige/teilweise
Standardabdeckung, Adapter und fehlende Entsprechungen. Keine eigene Funktion
wurde ohne Nutzerentscheidung stillgelegt. Die normale Lagentiefe ist ein
Standardkandidat; Projected, Normalentiefe, Wave Overhangs, Bestands-/Auftrags- und
HA-Autorität haben weiterhin eigene erforderliche Anteile.

## Offene Abnahme und Grenzen

- [ ] Einzelne neue Beta-Funktionen und deren Kombinationen im Nutzerworkflow prüfen;
  CSV-Zeilen bedeuten integrierter Quellstand, keine individuelle Freigabe jeder Funktion.
- [ ] Alle automatisch zusammengeführten gemeinsamen Pfade vollständig semantisch prüfen;
  unabhängiges Review und grüne Tests ersetzen diese vollständige Einzelprüfung nicht.
- [ ] Vollständige eigene Projekte und abhängige HA-Benutzerprofile an Kopien erproben.
- [ ] Nutzerentscheidung pro möglichem Orca-Ersatz dokumentieren.
- [ ] macOS/Linux/ARM-Pakete und vollständige CI prüfen.
- [ ] Externe Cloud-/Druckeragenten, installierte HA-Komponente sowie P2S/AMS-Druckabläufe
  getrennt abnehmen. Es wurden keine Druckaufträge oder Heizbefehle gesendet.
- [ ] Unveröffentlichte Änderungen des älteren Haupt-Arbeitsbaums separat abgleichen.

Es gibt keine Release-/Installationsfreigabe. Der produktive Checkout, seine
Konfiguration und seine Zugangsdaten werden durch diese lokale Testversion nicht ersetzt.
Rückweg: Testanwendung schließen und die bisherige Quack-Version weiterverwenden.

## Getroffene Entscheidungen

1. Separater verwalteter Arbeitsbaum statt Änderung des bereits veränderten Hauptcheckouts.
   Folge: getrennte Pfade und ein zusätzlicher lokaler Zweig; keine Übernahme fremder Änderungen.
2. Vorhandenes unterstütztes MSVC-Werkzeug statt Neuinstallation eines Compilers.
   Folge: Windows ist lokal geprüft, die übrigen Compiler-/Plattformkombinationen bleiben offen.
   Der fest gepinnte wxWidgets-Commit benötigt einen vollständigen statt flachen Git-Download.
3. Unentschiedene eigene Funktionen behalten und gemeinsame Farbtiefen wiederverwenden.
   Folge: eigener Wartungsaufwand bleibt bis zu einem belegten und gewählten Standardersatz.

Das unabhängige Review hat die zwei behobenen Sicherheits-/Integrationsgrenzen
gezielt geprüft; es hat vollständige Quellpfad-Parität, weitere Plattformen,
gerenderte komplette GUI-Flows, Cloud-Dienste und physische Akzeptanz ausdrücklich
nicht beurteilt. Diese Punkte bleiben offen und werden nicht als bestanden gewertet.
