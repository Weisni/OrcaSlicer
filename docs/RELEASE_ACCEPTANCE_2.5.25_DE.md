# Abschlussprüfung für QuackSlicer 2.5.25

Stand: 2026-10-30. Die Veröffentlichung ist nach erfolgreicher Abschlussprüfung
autorisiert. Dieser Bericht trennt bestandene lokale Prüfungen von noch offenen
Release- und Geräteprüfungen. Er ist kein Nachweis, dass jede denkbare Kombination
aller Funktionen und Druckermodelle geprüft wurde.

## Bereits geprüfte Bereiche

| Bereich | Tatsächlicher Nachweis | Grenze |
| --- | --- | --- |
| HA-Inventar, Profile, Konflikte, Aufträge, Kosten, Reservierungen und Preheat | 436 Python-Tests mit gebündeltem Python 3.12.13 bestanden; 11 JavaScript-Testdateien bestanden | Isolierte Testdaten/Loopback; kein physischer Druckauftrag |
| Testprofil und Zugangsdaten | Zwei Regressionstests bestanden; Zugangsdaten werden ausgeschlossen, Endpunkte validiert, Netzwerk bleibt initial deaktiviert | Kein Kopieren produktiver Credentials |
| Slicing und CLI | STL, Überhang, Support und Standard-3MF verarbeitet; native 3MF exportiert, erneut geladen und gesliced | Ausgewählte Modelle, keine vollständige Profilkombinatorik |
| Vergleich mit offizieller Orca-Beta | Gemeinsames P2S/PLA-Beispiel: 4.009 identische Bewegungsbefehle und berechnete 2,40 g | Keine allgemeine Gleichheits- oder Qualitätsbehauptung |
| Standard-3MF | Geometrie und Objektmaterial 1/2 sowie zwei Farben nach Roundtrip erhalten; ZIP/XML geprüft | Bestehende Formatregressionen ergänzen die Stichprobe |
| Systemprofile | Profilwerkzeug prüfte alle 71 Hersteller: null Dateien mit Fehlern oder Warnungen | Statische Profilprüfung; praktische Druckqualität bleibt offen |
| CAD in der Windows-Oberfläche | STEP mit 6.958 Dreiecken geladen, automatisch angeordnet und zusammen mit Projektmodellen gesliced; Vorschau mit 208 Schichten, 25,65 g und 2 h 1 min angezeigt | STEP-Import über CLI wird auch vom offiziellen Vergleichsprogramm nicht unterstützt |
| A4/A3 und virtuelle Mischung | Erneut nach vollständigem Neustart A4 auf Filament 1, A3 auf 2, ungebundene 50/50-Mischung auf 3; HA-Vergleich ohne Profil-/Farbabweichung und Erstprofil-Hinweis; Düsenwechsel 0,4 → 0,6 → 0,4 erhält Bindung und Mischung | GUI-Nachweis mit Lifecycle-Korrekturen; finale reguläre Versionskennung wird anschließend separat geprüft |
| Wave Overhangs und Support | Neue Regression reproduziert zwei Fehler vor Korrektur; danach 874 Assertions in 37 Fällen bestanden | Digitale Geometrie-/Pipelineprüfung, keine gedruckte Abnahme |
| Zusammengeführte Quellpfade | Verbleibende 52 automatisch zusammengeführte Pfade einzeln semantisch geprüft, zusätzlich sieben kritische Pfade | Statische Prüfung ersetzt keine Plattformausführung |
| Release-Gate | Elf Fixturetests, vier Workflows mit Actionlint, YAML/XML und Bash-Syntax bestanden | Echte vollständige CI-Pakete und deren Upload noch offen |

Die CAD-Prüfkopie liegt unter
`build-test/evidence/release-gui-cad-with-project.3mf`. Die vom Benutzer gewünschte
A4/A3-Ausgangskopie `build-test/comparison/a4-a3-mixed-test.3mf` wurde dabei nicht
überschrieben.

## In dieser Prüfung korrigierte Befunde

- Teilweise Wave-Abdeckung entfernte bisher auch unbedeckte Überhänge und
  Cantilever aus der Tree-Support-Berechnung. Jetzt wird nur die tatsächlich
  abgedeckte Fläche abgezogen; deaktivierte Pfade bleiben erhalten.
- Der native Mehrdrucker-Versand besitzt keine HA-Tickets für Reservierung und
  Preheat. In HA-/Demo-Materialmodus muss der Einzeldrucker-Dialog verwendet
  werden. Die Mehrdrucker-Pfade werden vor dem Versand gesperrt.
- Die zusätzliche Prüfung fand denselben fehlenden Buchungspfad auch ohne HA:
  Lokale Reservierung, Auftragszuordnung und passives Vorheizen werden vom
  nativen Mehrdruckerpfad umgangen. Deshalb ist vor Export eine ausdrückliche
  Entscheidung für ungetracktes Drucken nötig, standardmäßig „Cancel“.
  Die Zustimmung wird pro Aufgabe gespeichert und erneut vor dem Versand
  geprüft. Aktives passives Vorheizen sperrt den Mehrdruckerpfad unabhängig
  von HA. Gezielte Tests reproduzierten acht Fehler vor der Korrektur und
  bestehen danach mit 90 Assertions in neun Fällen. Der vollständige Nachlauf
  besteht mit 3.090 Registrierungen; die erneute CI-Qualifikation bleibt offen.
  Die GUI-Sperre wurde auch bei ausgeschalteter HA-Anbindung und aktivem
  passivem Vorheizen vor dem Export bestätigt.
- Linux-AppImage-Suche und Flatpak-Metadaten verwendeten teilweise den
  Upstream-Namen. D-Bus- und macOS-Bundle-Identitäten sind jetzt Quack-spezifisch;
  die lokale Testanwendung besitzt zusätzlich eine eigene Identität.
- macOS-Plist: Executablename und Schlüssel-/Wertstruktur korrigiert.
- Die Veröffentlichung prüft die tatsächliche, SHA-gebundene Buildmatrix,
  vollständige Architekturpakete, Installer-Prüfsummen und hochgeladene Digests.
  Unvollständige Self-hosted-Matrizen qualifizieren kein vollständiges Release.

## Offene Abschlussgates

- [x] Finaler Windows-Release-Build mit allen Korrekturen: 3.090 native
  Registrierungen ohne Fehler, neun Umgebungs-Skips; 51 geänderte
  C++-Dateien bestehen Clang-tidy. Reguläre EXE-/Produktversion ist 2.5.25.
- [x] Aktualisierte lokale Testversion gestartet: A4 auf 1, A3 auf 2 und
  virtuelle 50/50-Mischung auf 3 erhalten. Vorheriger HA-/Düsen-Roundtrip
  bestätigt keine ausstehenden Profil-/Farbänderungen im Ausgangskontext.
- [x] Mehrdrucker-Sperre im GUI mit credentialfreiem Loopback-Profil bestätigt;
  Queue-/Dispatch-/Lifetime-Regressionen bestehen mit 90 Assertions in neun
  Fällen. Bereits wartende Aufträge werden beim HA-/Vorheiz-Moduswechsel
  abgewiesen; ausdrücklich bestätigte ungetrackte Sends bleiben parallel möglich.
- [ ] Vollständige CI des tatsächlich veröffentlichten Commits auf allen
  unterstützten Plattform-/Architekturlanes.
- [ ] Endgültige Pakete, eingebettete Versionen und veröffentlichte Prüfsummen
  gegen genau diesen Build prüfen.

Die lokale Windows-Buildskript-Testsuite bestand bisher 192 Fälle; 30 Fälle
benötigen hier nicht installierte Clang-Toolchains und sind lokal fehlgeschlagen.
Die ExceptionThrowInfo-Prüfung bestand 9/9 Fälle. Fehlende Voraussetzungen werden
nicht durch übersprungene oder deaktivierte Tests verdeckt; die passende
Windows-CI muss die vollständige Suite bestehen.

Der vollständige native Zwischenlauf nach den Lifecycle-Korrekturen bestand
mit 3.085 Registrierungen und null Fehlern (neun Umgebungs-Skips). Der Benutzer
wählte anschließend ausdrücklich das reguläre Release 2.5.25. Der finale Build
mit dieser Versionskennung und den zusätzlichen Desktop-Policytests besteht
mit 3.088 Registrierungen in 68,18 Sekunden. Nach der zusätzlichen Absicherung
des lokalen Mehrdruckerpfads bestehen 3.090 Registrierungen in 76,50 Sekunden,
null Fehler und neun Skips. Clang-tidy besteht für alle 51 geänderten C++-Dateien.

Aktuelle lokale Test-DLL SHA-256:
`30a73e642c02086cb8b289c728b1349459d456e71db0169a4970cd3d4f1e26b0`.
Aktuelle lokale Test-EXE SHA-256:
`1d81bf0b0ec9f2e78c1bd8fa7f3d1366edddeaed663a13c9e27daec8eec69741`.
Diese Dateien verwenden weiterhin ausdrücklich die isolierte Identität
„QuackSlicer Beta Test“; produktive Releasepakete werden getrennt durch CI gebaut.

Netzwerk-Reload und normales Schließen werden während aktiver nativer Transfers
abgelehnt. Ein nicht verhinderbares Beenden durch das Betriebssystem beendet den
Prozess ohne gleichzeitiges Zerstören eines noch verwendeten SDK-Objekts; der
physische Ausgang eines solchen unterbrochenen Versands bleibt ungewiss. Es gibt
keinen automatischen erneuten Versand und keine erfundene Verbrauchsbuchung.

Live-HA-Installation, physisches Heizen/Drucken, externe Cloud-/Agenten-Dienste
und die praktische Druckqualität wurden durch diesen Auftrag nicht freigegeben
und sind keine bestandenen Abnahmen. Experimentelle Orca-Funktionen behalten
ihre Einschränkungen. Keine benutzereigene Funktion wurde durch eine vermeintlich
gleichwertige Orca-Standardfunktion entfernt.

Detailbelege liegen lokal unter `build-test/evidence`, insbesondere
`release-ha-python-qualified.log`, `release-ha-js-summary.log`,
`release-cli/verified-results.json`, `release-profile-tool.log`, `support-review-green-workspace-tmp.log`,
`release-critical-shared-review.md`, `release-remaining-shared-review.md` und
`release-pipeline-audit.md`. Testdaten und Zugangsdaten gehören nicht ins Release.
