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
| Release-Gate | Zehn Tag-/Target- und zwölf Asset-Fixturetests, vier Workflows mit Actionlint, YAML/XML und Bash-Syntax bestanden | Echte vollständige CI-Pakete und deren Upload noch offen |

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

Der erste vollständige Plattformlauf am Zwischenstand `5d46bc25ca` fand unter
Linux ARM64 mit Clang 18 zwei `unused-private-field`-Fehler im G-Code-
PreCoolingInjector. Beide Eingaben werden derzeit bewusst nicht ausgewertet.
Sie sind gezielt mit `[[maybe_unused]]` markiert, entsprechend dem bestehenden
Muster in `FanMover.hpp`; globale Warnungen bleiben unverändert. Die echte
Übersetzungseinheit reproduziert lokal beide Compilerfehler vor der Korrektur
und besteht danach mit aktivem `-Werror=unused-private-field`. Ein vollständiger
neuer CI-Lauf am korrigierten Commit ist weiterhin erforderlich.

Der nächste Plattformlauf an `3ba61cff19` fand anschließend einen unbenutzten
Lambda-Capture in der Kundensummierung. Eine zusätzliche vollständige Clang-
Prüfung aller 32 geänderten Übersetzungseinheiten fand außerdem zwei weitere
unbenutzte Captures im AMS-Button und 3MF-Fortschrittscallback. Alle drei sind
entfernt; die vollständige Prüfung besteht mit beiden Warnungskategorien als
Fehler. Die benötigte `this`-Bindung des AMS-Buttons bleibt erhalten. Die
Lambdabodies und Summenberechnung wurden nicht geändert. Ein unabhängiges
Source-Review bestätigt die tatsächlichen drei Stellen. Der neue Plattformlauf
muss diesen Stand noch vollständig bestätigen.

Die Prüfung wurde anschließend auf alle 127 im Windows-Testbuild gebauten
Quack-Übersetzungseinheiten gegenüber der offiziellen Beta erweitert, jeweils
ohne vorübersetzte Header. Sie fand drei weitere unbenutzte Captures, vier
ungenutzte Objektfarb-Dialogfelder sowie fehlende direkte Header. Die Captures
sind entfernt; Felder, Typen, Reihenfolge und Konstruktorbindungen bleiben mit
gezielten Attributen erhalten. Die direkten wx-/Kompatibilitätsheader sind
ergänzt. Alle 127 Einheiten bestehen danach die vollständige Syntaxprüfung mit
den beiden Warnungskategorien als Fehler; alle 58 geänderten C++-Dateien
bestehen die Projekt-Clang-tidy-Prüfung. Dies ersetzt weiterhin keine Linux-/
macOS-Ausführung. Belege: `all-quack-clang-warnings-summary-green.log`,
`release-all-quack-tidy-final.log` und `obj-color-unused-{red,green}.log`.

Der Lauf an `140693c81a` erreichte anschließend das GUI-Modul und fand eine
PCH-spezifische Kollision der tatsächlichen Git-Kennung mit dem Platzhalter
`0000000`. Der zentrale Headergenerator entfernt jetzt die vorigen Hash-/
Suffix-Definitionen unmittelbar vor den autoritativen erzeugten Werten. Das
gilt auch für den Windows-Crashbericht; PCH-Policy und Warnungsflags bleiben
erhalten. Die echte BuildCommit-Quelle reproduziert den Fehler sowohl in
Clang als auch in einer tatsächlichen PCH-Fixture mit `/W4 /WX`; beide Proben
bestehen nach der Korrektur. Die Fixture prüft zusätzlich die echte Kennung
und das Dirty-Label zur Laufzeit. Belege:
`git-metadata-{pch,real-pch}-{red,green}.log`. Der neue vollständige
Plattformlauf bleibt ein erforderliches Gate.

Windows ARM64 meldete am selben Zwischenstand außerdem eine aktuelle Clang-
Diagnose für implizite Copy-Zuweisungen zweier Schnittverbinder-Typen mit
eigenen Copy-Konstruktoren. Beide erhalten explizit `operator= = default`;
damit bleibt die vorherige memberweise Zuweisung erhalten, einschließlich
Layout, Serialisierung und bereits bestehender Move-Unterdrückung. Die echte
GLGizmoCut-Übersetzungseinheit reproduziert beide Fehler vor der Korrektur und
besteht danach mit identischem `-Werror`. Die 127 vollständigen Quack-Einheiten
bestehen zusätzlich mit dieser dritten Diagnosekategorie als Fehler; die
Projektprüfung besteht für 59 geänderte C++-Dateien. Belege:
`cut-copy-clang-{red,green}.log`, `all-quack-clang-copy-summary.log` und
`release-copy-tidy.log`. Die bestehenden `[CutConnectors]`-Geometrieprüfungen
werden mit dem nachfolgenden vollständigen Neubau ausgeführt.

Die erweiterte Clang-Prüfung umfasst alle 739 aktuell kompilierten eigenen
Anwendungs-Übersetzungseinheiten, einschließlich unveränderter Beta-CAD-Dateien.
Sie behandelt vier konkrete Warnungskategorien als Fehler: unbenutzte private
Felder und Lambda-Captures, veraltete implizite Copy-Zuweisungen sowie abweichende
class/struct-Deklarationen. Die CAD-Vorwärtsdeklaration von Camera entspricht
jetzt dessen tatsächlicher struct-Definition. Im Gesamtlauf bestehen 738 Einheiten;
die einzige fehlerhafte Einheit meldet zwei unbenutzte CAD-Edge-Body-Felder.
Gezielte Attribute erhalten deren Layout und Initialwerte; die tatsächlich
verwendeten Nachbarfelder bleiben unverändert. Der erneute Lauf dieser Einheit
besteht danach mit denselben vier Fehlerkategorien. Die reguläre Projektprüfung
besteht für alle 61 geänderten C++-Dateien. Diese Prüfung ist keine Aussage über
sämtliche anderen Clang-Warnungen. Belege: `all-app-clang-warnings-summary.log`,
`cad-panel-unused-green.log`, `release-cad-tidy-final.log`.

Der lokale Neubau an `aeba440219` besteht mit 3.090 nativen Registrierungen,
null Fehlern und neun Umgebungs-Skips einschließlich aller drei bestehenden
CutConnectors-Geometriefälle. Der erneute CLI-Test entspricht mit 4.009
Bewegungsbefehlen der offiziellen Beta. Nach normalem Neustart bestätigt die
GUI erneut A4 auf 1, A3 auf 2, die virtuelle 50/50-Mischung auf 3 sowie einen
leeren ausgehenden HA-Vergleich ohne Erstprofil-Hinweis. Im tatsächlichen
Objekt-Kontextmenü heißt Platz 3 „Mixed Filament 3“. Es wurde nichts an HA
publiziert und kein physischer Auftrag gestartet.

Der wiederholte Vier-Diagnosen-Gesamtlauf besteht anschließend für alle 739
Einheiten. Die echte Plattform-CI findet danach noch eine zusätzliche C++17-
Inkompatibilität in der physischen Filamentzuordnung: Ein Lambda erfasst direkt
das Structured Binding eines Map-Schlüssels. Der Referenz-Initcapture erfasst
denselben stabilen Schlüssel C++17-konform; der synchrone `find_if`-Aufruf,
Aggregation und Bestandsvergleich bleiben gleich. Die echte Übersetzungseinheit
reproduziert den Fehler vor der Korrektur und besteht danach; das unabhängige
Review bestätigt unveränderte Lebensdauer und Zuordnung.

Der erweiterte lokale Gesamtlauf umfasst ebenfalls alle 739 Einheiten und
verwendet `-Wall -Werror` mit den bestehenden CI-Ausnahmen. Die GNU-Clang-
Optionen werden bei clang-cl über `/clang:` weitergereicht; dessen eigene
`-Wall`-Option hätte stattdessen auch C++98-Kompatibilitätswarnungen aktiviert.
Die im lokalen LLVM 22 unbekannte enum-constexpr-Demotion entfällt nur lokal.
731 Einheiten bestehen einschließlich zusätzlich aktivierter C++20-Diagnosen;
acht Einheiten melden dabei ausschließlich zusätzliche Attribut-/Variadic-Macro-
Diagnosen aus dieser erweiterten Gruppe. Alle acht bestehen den gezielten
erneuten Lauf mit dem tatsächlichen CI-Warnumfang ohne diese Zusatzgruppe.
Die unveränderte TextureToColor-Datei wurde auch in der echten Linux-CI mit
Clang 18 erfolgreich kompiliert. Es werden weder Upstream-Attribute/Makros noch
die produktive Warnpolicy geändert. Die reguläre Projektprüfung besteht für
62 geänderte C++-Dateien. Vollständige Plattform-CI bleibt das Freigabe-Gate.
Belege: `allocation-cpp17-{red,green}.log`,
`all-app-clang-final-65e4-summary.log`, `all-app-clang-ci-warnings-summary.log`,
`all-app-ci-default-followup-summary.log`, `release-cpp17-tidy.log`.

- [x] Lokaler Windows-Release-Abnahmesnapshot an `c14cbf0735`: 3.090 native
  Registrierungen ohne Fehler, neun Umgebungs-Skips; 52 geänderte
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
null Fehler und neun Skips. Der neue Build an `c14cbf0735` nach der Clang-
Korrektur besteht erneut mit 3.090 Registrierungen in 78,87 Sekunden.
Clang-tidy besteht für alle 52 geänderten C++-Dateien. Der erneute CLI-Vergleich
liefert dieselben 4.009 Bewegungsbefehle wie die offizielle Beta; der frische
HA-Vergleich nach Neustart bestätigt erneut A4 auf 1, A3 auf 2, die virtuelle
50/50-Mischung auf 3 und keine Profil-/Farbabweichung oder Erstprofil-Warnung.

Lokaler DLL-Abnahmesnapshot an `c14cbf0735`, SHA-256:
`922e0fd85dae22d7b0e19420a7d22d0a3e8943f7c5e450ec0c286818369cd81a`.
Zugehörige lokale Test-EXE SHA-256:
`1d81bf0b0ec9f2e78c1bd8fa7f3d1366edddeaed663a13c9e27daec8eec69741`.
Diese Dateien verwenden weiterhin ausdrücklich die isolierte Identität
„QuackSlicer Beta Test“; produktive Releasepakete werden getrennt durch CI gebaut.
Spätere lokale Neubauten werden im lokalen `build-manifest.json` sowie in
`build-test/evidence` mit tatsächlichem Commit und Prüfsummen dokumentiert.

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
`release-cli-2.5.25/verified-results.json`, `final-c14-cli-smoke.log`,
`release-c14-full-ctest.log`, `release-clang-inputs-tidy.log`,
`release-profile-tool.log`, `support-review-green-workspace-tmp.log`,
`release-critical-shared-review.md`, `release-remaining-shared-review.md` und
`release-pipeline-audit.md`. Testdaten und Zugangsdaten gehören nicht ins Release.
