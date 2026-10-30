# HA-Abnahme der lokalen Beta-Testversion

Prüfumfang: Materialauswahl, physische Rollen-UUID, Profilübernahme in beide
Richtungen, lokale Overrides, Nozzle-/Flow-Kontext und erneuter Programmstart.
Produktive HA-Schreibzugriffe, Installation, Druck und Heizen gehören nicht
zu dieser Prüfung. Fehlerfälle und Profilschreibvorgänge verwenden ausschließlich
einen Loopback-Dienst mit synthetischer Datenbank.

## Prüfaufbau

- Lokales Paket: `build-test/QuackSlicer-Beta-Test`.
- Benutzerzustand vor den Prüfungen gesichert als
  `build-test/comparison/ha-full-audit-user-backup.3mf`, einschließlich der
  aktuellen Texturänderungen, sechs Materialindizes und 0,6-mm-Konfiguration.
- Separates Mock-Datenprofil: `build-test/ha-audit-profile`; keine Übernahme des
  produktiven Zugangsspeichers oder der Geschäftsdatenbank.
- Lokaler Testdienst: `127.0.0.1:18765`, Datenbank unter
  `build-test/ha-audit-fixture-v2`, drei synthetische PLA-Rollen, jeweils vier
  Profile für 0,4/0,6 mm und Standard/High Flow. Die Standardprofile verwenden
  das bisherige 149-Feld-Schema, High Flow das aktuelle 151-Feld-Schema.
- HTTP-Protokoll erfasst ausschließlich Operation, Status und Test-Request-Key.
  Es enthält keine Tokens. Der Dienst besitzt keine Druckerschnittstelle.

## Nachgewiesene Korrekturen

1. Doppelte Flow-Auswahl: Der Quack-Zusatz im Variant-Kasten konkurrierte mit
   der nativen Beta-Nozzle-Karte und aktualisierte das Plate-Mapping nicht.
   Die native Auswahl bleibt einschließlich Gerätegrenzen und Mapping erhalten.
2. Zugangspersistenz: Unterschiedliche Schreibweisen desselben Datenpfads
   erzeugten unterschiedliche Zugangsschlüssel. Die Pfadidentität wird jetzt
   normalisiert; passende alte Einträge bleiben erhalten und werden nach einem
   erfolgreichen Ladevorgang in den stabilen Slot übernommen.
3. Profil-Publish: Bestätigte wiederholte Requests aktivieren dieselben
   eingefrorenen Änderungen wie der normale Weg. Identische Verwendungen einer
   Rollen-UUID werden gemeinsam aktualisiert; abweichende Overrides bleiben lokal.
4. Batch-Import: Ein bearbeitetes kanonisches HA-Profil muss beim Wechsel auf
   einen bereits gemeinsam verwendeten Inhalt einen eigenen Projekt-Editbuffer
   behalten. Der frühere Guard blockierte stattdessen den gesamten Import.
5. Native Variantenwerte: Orca erweitert einzelne Werte auf mehrere
   Extrudervarianten. Hashgeprüfte, effektiv identische Erweiterungen gelten
   jetzt auch beim Düsenwechsel als unverändert. Echte Werte-, Nil- und
   Abhängigkeitsänderungen bleiben geschützt. Nullable Bool-Felder verwenden
   bei kurzen Vektoren denselben Index-0-Fallback wie die Konfiguration.
6. Offline-Anzeigenamen: Das Frischelimit von 300 Sekunden gilt weiterhin für
   Auswahl und Sync. Bereits bestehende Projektbindungen erhalten ihren Namen
   unabhängig davon aus dem verifizierten Profilcache. Ein veralteter Roster
   autorisiert keine neue Rolle oder Druckfreigabe.
   Auch der Aufruf in der Filamentanzeige liegt jetzt außerhalb des Live-Roster-
   Gates; Slot, Produkt und Badge benötigen weiterhin aktuelle Bestandsdaten.
   Die zwischenzeitliche Cachepfad-Vermutung wurde durch native Gegenproben
   ausgeschlossen. Der Cache-Konstruktor wurde deshalb nicht geändert.

## Prüfergebnisse

| Ablauf | Stand und Beleg |
| --- | --- |
| Bestehender HA-Bestand und sechs montierte Plätze | Im bisherigen Testprofil lesend bestätigt; keine produktiven Profilschreibvorgänge |
| Einzelzuordnung einer Mock-Rolle | A1 auf Material 1 mit lesbarem Profilnamen erfolgreich |
| Zwei Rollen mit identischem Profilinhalt | A1 und A3 gemeinsam auf Material 1/2 importiert; lesbare Namen und unterschiedliche UUIDs erhalten |
| Rollenwahl im Dropdown | Material 1 von A1 auf A2 gewechselt; Material 2 bleibt A3, kein Profilupload |
| Standard-/High-Flow-Wechsel | Beide gebundenen Rollen wechseln automatisch auf die passende Profilbasis; nur eine Flow-Auswahl |
| Düsendurchmesser | 0,4 → 0,6 → 0,4 mm geprüft; passende Profilbasis und UUIDs erhalten, falscher Transferdialog beseitigt |
| Profilupload, Konflikt, verlorene Antwort und Retry | Vollständige GUI-Kette im Mock bestanden, Details unten |
| Gespeicherter Token nach Neustart | Echtes Testprofil mit Backslashes und `..` im Datenpfad gestartet; sechs Live-HA-Plätze ohne Tokeneingabe lesend geladen |
| Offline-Neustart | Endgültiger Build zeigt beide gespeicherten Profilnamen nach Neustart ohne Dienst bei über 18 Minuten altem Roster; UUIDs und Profilwerte erhalten, keine veralteten Slot-Badges |
| Backend und Weboberfläche | 436 Python-Tests sowie 11 JavaScript-Testdateien bestanden |
| Native Gesamtsuite | 3.063 registrierte Tests, null Fehler, neun Umgebungsskips |
| Review-Nachtrag | Offline-Namensauflösung verlangt die veröffentlichte gespeicherte Düsenvariante; passendes Basisprofil reicht nicht. RED mit zwei erwarteten Fehlern, GREEN mit 116 Assertions in 21 Binding-Tests |
| clang-tidy | 26 geänderte C++-Dateien bestanden, einschließlich des letzten GUI-Aufruf-Fixes |

## Upload und Wiederholung

Eine lokale Flow-Ratio-Änderung von 1,02 auf 1,03 wurde ausdrücklich über
Projekt → HA bestätigt. Der synthetische Server erzeugte zunächst HTTP 409:
Die lokale Änderung blieb erhalten und es entstand kein Wiederholungsjournal.
Danach übernahm der Server die Änderung, ließ aber die Antwort ausfallen.
Quack behielt den bestätigten Request im Journal. Die bewusste Wiederholung
verwendete denselben Request-Key, übernahm den bestätigten HA-Stand und löschte
das Journal. Die Datenbank enthält genau eine entsprechende Profilmutation.
Alle drei Testrollen behielten jeweils 500.000 mg; keine Slot-, Bestands- oder
Reservierungsänderung wurde durch diesen Profilupload ausgelöst.

Native Regressionen prüfen zusätzlich mehrere Materialindizes derselben UUID,
abweichende lokale Overrides und isolierte Editbuffer bei gemeinsamem Inhalt.
Diese Randfälle sind automatisierte Softwarebelege; sie wurden nicht alle als
separate manuelle GUI-Kette wiederholt.

Die Protokolle liegen unter `build-test/evidence/ha-full-*`. Temporäre
Fixturefehler wurden getrennt korrigiert: vollständige optionale Native-Felder
und unveränderte gültige ConfigOption-Serialisierung. Sie wurden nicht durch
Aufweichen der produktiven Graph- oder Profilprüfung umgangen.

## Auslieferungsnachweis

Die nachfolgenden Hash-/Testangaben dokumentieren den HA-Abnahmestand. Der spätere
[Mixed-Filament-Nachtrag](QUACK_BETA_LOCAL_TEST_2026-10-30_DE.md#nachtrag-mixed-filament-beim-düsenwechsel)
nennt den inzwischen aktualisierten Runtime-Hash und die aktuelle Gesamtabnahme.

Das lokale Paket enthält die endgültige Release-DLL mit SHA-256
`c71a78022e6aca15e67a6b9eb1f4aa692f64b9a1dce342b6a65424fe18cc0056`.
Start: `build-test/QuackSlicer-Beta-Test/Start-Quack-Beta-Test.cmd`.
Der Starter verwendet weiterhin ausschließlich `build-test/test-profile` und
den getrennten Test-Zugangsspeicher. Die Mock-Daten bleiben separat; der lokale
Testdienst ist zum Abschluss beendet.

Belege:

- `ha-full-release-offline-final.log`: Release-Anwendungsbuild.
- `ha-full-clang-tidy-offline-final.log`: 26 geänderte C++-Dateien bestanden.
- `ha-full-ctest-offline-final.log`: vollständiger nativer Abschlusslauf.
- `ha-full-python-final.log`, `ha-full-js.log`: Backend und Weboberfläche.
- `ha-full-profile-preparation-final.log`: zwei Tests zur Testprofil-Erstellung.
- `offline-context-red.log` / `offline-context-green.log`: enger Review-Nachtrag.
- `ha-full-acceptance-summary.json`: DLL-/Backup-Hashes, gespeicherte synthetische
  Projektbindungen, unveränderte Testbestände und idempotente Requestfolge.

Der Benutzer-Backup-Hash ist unverändert
`f2a0ef9bc329a4a2f54c354732fbee5e2e91d55a893823953561db62a734ef3e`.
Es wurde kein Produktionsprogramm installiert, kein Release veröffentlicht und
keine Änderung in die produktive HA-Datenbank geschrieben.

Ein erfolgreicher Softwaretest ersetzt keine Prüfung eines echten Drucks,
physischer AMS-Zuordnung oder thermischer Abläufe. Live-HA-Uploads wurden
ausschließlich durch den lokalen synthetischen Dienst geprüft. Damit ist keine
neue installierte HA-Version und keine physische Druckkette abgenommen.

## Nachtrag: A3-Farbe und virtuelle Materialplätze

Der gesicherte Benutzerzustand `build-test/comparison/a3-color-user-state.3mf`
enthielt zwei physische Materialplätze. Platz 2 war zugleich A3 zugeordnet und
mit `filament_is_mixed = 1` als Mischung aus `1,2` markiert. Diese Selbstreferenz
ließ die Mischberechnung die weiße A3-Farbe `#FFFFFF` mit `#79879E` überschreiben.
Der Farbvergleich zeigte deshalb eine tatsächlich lokal veränderte Farbe.
Die Sonderstellung des zweiten Platzes erklärt, warum andere Rollen unauffällig waren.
Der vom Benutzer bestätigte Auslöser ist die Auswahl von A3 im Materialmenü.

Die Korrektur trennt physische und virtuelle Plätze auch beim HA-Anfügen und
beim Wiederherstellen von Druckersnapshots. Virtuelle Plätze sind keine HA-Ziele
und keine Quelle für Farb-/Profiluploads. Eine Umordnung berücksichtigt unter
anderem Stützmaterial, Höhenbereiche, bemalte Flächen, Toolwechsel und
Plattenreihenfolgen; fehlerhafte Metadaten werden vor der Übernahme abgelehnt.
Das Laden eines Projekts ohne Mischfelder übernimmt keine alten Mischreste.

Die getrennte Kopie `build-test/comparison/a3-color-repaired.3mf` wurde aus dem
gesicherten Zustand erstellt. A3 wurde anhand seiner unveränderten UUID auf
Weiß zurückgesetzt, die ungültige Doppelrolle entfernt und anschließend im GUI
die vorhandene Definition 1+2 mit 50/50 als eigener Platz 3 neu angelegt.
Gespeichert sind Farben `#161616`, `#FFFFFF`, `#79879E`, Kennzeichen `0,0,1`
und Komponenten `1,2` ausschließlich auf Platz 3. Modellgeometrie und bestehende
Modellzuweisungen wurden beibehalten; die Download-Originaldatei blieb unberührt.

Im aktualisierten GUI wurde A3 erneut aus dem live gelesenen Materialmenü
gewählt, auch bei bestehender Mischung. Weiß blieb erhalten. Nach Speichern,
Beenden und Neustart wurden A3 und Mischung wieder korrekt angezeigt. Der
Vergleich Projekt → HA bot keine A3-Farbänderung an. Es wurde nichts hochgeladen.

Offen bleibt eine separate Profilabweichung für A3, die nach dem Neustart der
wiederhergestellten Kopie angeboten wird. Außerdem zeigt der erste Materialplatz
einen Copper-PLA-Namen bei einer bestehenden ASA-Inventarbindung. Diese älteren
Projektzuordnungen wurden weder stillschweigend geändert noch veröffentlicht.
Die Farbkorrektur ist damit geprüft; eine vollständige unveränderte
Profilgleichheit dieser wiederhergestellten Benutzerdatei ist nicht behauptet.

Aktueller Release-Nachweis:

- `a3-complete-core-green.log`: 77 gezielte Fälle, 962 Assertions bestanden.
- `a3-color-ctest-final.log`: 3.077 Registrierungen, null Fehler, neun
  umgebungsbedingt übersprungene Tests.
- `a3-color-clang-tidy-final.log` und `a3-color-new-files-clang-tidy.log`:
  35 geänderte sowie zwei neue C++-Dateien bestanden.
- `a3-color-release-final.log`: Release-Anwendungsbuild bestanden.
- Die unveränderten HA-Backend-/Webtests bleiben im vorherigen Nachweis
  dokumentiert; sie wurden für diese C++-Korrektur nicht erneut ausgeführt.

Aktuelle `runtime/OrcaSlicer.dll`, ersetzt alle früheren Hashangaben:
`7203b7b7ad17a0b0c5106c9f3444f1e16fe7eec89fe0135d9972f82e4e726058`.
Starter und getrenntes Testprofil bleiben unverändert.

## Nachtrag: Profilgleichheit nach Neustart

Die zuvor offene A3-Profilabweichung ist korrigiert. Einige ältere Profile
speichern einen gemeinsamen Wert für Standard/High Flow, andere bereits zwei
Werte. Beim Export wurden diese Listen ohne vorherige Erweiterung verkettet.
In der betroffenen Kopie standen für drei Materialien mit sechs Varianten zum
Beispiel nur fünf Pressure-Advance-Werte. Dadurch konnte der erneute Import
Werte vom benachbarten Material übernehmen. Das war eine tatsächliche
Parameterabweichung und keine bloße Anzeigeabweichung.

Beide Wege zur Erstellung der vollständigen Projektkonfiguration erweitern
gemeinsame Werte jetzt pro Profil vor der Verkettung. Normales Slicing mit
bereits ausgewählter Variante bleibt unverändert. Beim Import wird das
gespeicherte HA-Profil nur bei verifizierter Inhaltsidentität und Gleichheit
aller Materialwerte wiederverwendet. Dabei werden vom kombinierten Projektformat
nicht transportierte Kompatibilitätslisten ausschließlich für diesen Nachweis
aus dem gespeicherten Profil ergänzt. Echte lokale Änderungen verhindern den
Abgleich und bleiben erhalten.

Der neue Regressionstest reproduzierte zunächst 65 fehlgeschlagene Assertions.
Er prüft unterschiedliche Listenlängen, beide Exportwege, den unveränderten
Import und eine echte lokale Pressure-Advance-Änderung. Der vollständige
HA-/Mixer-/Zuordnungs-Teillauf besteht mit 79 Fällen und 1.241 Assertions.
Alle 3.078 nativen Registrierungen bestehen ohne Fehler, neun Tests sind
umgebungsbedingt übersprungen. Clang-tidy besteht für 35 geänderte Dateien.
Eine unabhängige statische Codeprüfung ergab keine weiteren Befunde.

Die neue Kopie `build-test/comparison/a3-profile-repaired.3mf` wurde im GUI
mit dem gespeicherten HA-Profil für A3 synchronisiert, gespeichert, die Anwendung
beendet und neu gestartet. Danach bot der Vergleich Projekt → HA für A3 weder
Profil- noch Farbänderungen an. Die 50/50-Mischung blieb auf Platz 3. In der
gespeicherten Datei stehen Pressure-Advance-Werte korrekt als
`0.02,0.02,0.018,0.018,0.02,0.02`; die A3-Starttemperaturen bleiben `220,230`.
Das frühere `a3-color-repaired.3mf` und die zusätzliche Sicherung
`a3-profile-before-fix.3mf` bleiben erhalten. Bereits uneindeutig gespeicherte
Werte werden nicht automatisch erraten oder als vermeintliche lokale Edits
veröffentlicht; hier erfolgte die Wiederherstellung explizit aus HA in die Kopie.

Der verbleibende Eintrag für Filament 1 meldet weiterhin ein fehlendes vollständiges
HA-Profil für diesen Drucker-/Düsenkontext. Seine bestehende ASA-Inventarbindung
und abweichende Copper-PLA-Anzeige wurden nicht eigenmächtig ersetzt. Dies ist
getrennt von der nun behobenen A3-Abweichung zu entscheiden.

Belege unter `build-test/evidence`:

- `profile-restart-red.log`, `profile-restart-focused.log` und
  `profile-restart-ctest.log`: Regression und abschließende Tests.
- `profile-restart-release-final.log`: abschließender Release-Build.
- `profile-restart-clang-tidy.log`: Header-/Quellprüfung.

Aktuelle ausgelieferte Release-DLL (ersetzt alle früheren Hashangaben):
`cb7a9b6a6c81984e7e77053d3a4462b00fa978ac7380b4211b10662c3ed0989d`.
Es wurde nur die isolierte lokale Testversion aktualisiert. Live-HA wurde gelesen,
aber kein Profil veröffentlicht, kein Bestand verändert und kein Druck gestartet.

## Nachtrag: A4-Zuordnung und Mischungsbezeichnung

Der Benutzer hat Filament 1 inzwischen selbst auf A4 umgestellt und anschließend
ausdrücklich bestätigt, dass A4 erhalten bleiben soll. Die frühere Copper-/ASA-
Entscheidung ist für diese Testkopie damit erledigt. A4 besitzt bereits ein
vollständiges HA-Profil für P2S / 0,4 mm / High Flow; ein Erstprofil-Upload ist
hier nicht erforderlich.

Der aktuelle ungespeicherte Benutzerstand wurde vor dem Runtime-Austausch mit
dem GUI unter `build-test/comparison/a4-a3-mixed-test.3mf` separat gesichert.
Die bestehende Datei `a3-profile-repaired.3mf` wurde nicht überschrieben.
Die neue Datei speichert physisch A4 auf Index 1 und A3 auf Index 2, während
Index 3 ausschließlich die virtuelle Mischung 1+2 mit 50/50 enthält und keine
HA-Rollenbindung besitzt. Modelländerungen und bestehende Zuweisungen aus dem
aktuellen Benutzerstand wurden mitgespeichert.

Nach Austausch der lokalen Release-DLL und vollständigem Neustart wurde die
Kopie wieder geöffnet. Der HA-Dialog zeigt A4 → Filament 1 und A3 → Filament 2.
Im Vergleich Projekt → HA steht ausschließlich die Meldung, dass keine Profil-
oder Farbabweichungen vorliegen. Der Erstprofil-Hinweis ist verschwunden.
Der Objekt-Kontextmenüpunkt „Change Filament“ zeigt die virtuelle dritte Auswahl
als „Mixed Filament 3“, ohne internen HA-Profilhash oder angesammelte Dateisuffixe.
Beide Material-Kontextmenüs verwenden dieselbe Anzeigehilfe; die Materialindizes
und die Mischdefinition bleiben unverändert.

Zusätzlich verhindert die Vergleichsprüfung jetzt, dass beispielsweise ein
PLA-Profil als fehlendes Erstprofil einer gebundenen ASA-Rolle angeboten wird.
Eine solche Kombination erhält eine gesperrte Zeile „Check roll assignment“
mit Zuordnungshinweis. Der Regressionstest scheiterte vor der Änderung mit zwei
Assertions und besteht danach. Die GUI-Anzeige unterdrückt in diesem Fehlerfall
den irreführenden Erstprofiltext. Gültige Erstprofile bleiben ausdrücklich
auswählbar; es wird keine automatische Veröffentlichung eingeführt.

Abschlussbelege unter `build-test/evidence`:

- `filament1-red.log`, `filament1-green.log`: Regression; 80 gezielte Fälle mit
  1.244 Assertions bestanden.
- `filament1-ctest-final.log`: 3.079 native Registrierungen, null Fehler,
  neun umgebungsbedingt übersprungene Tests.
- `filament1-release-final.log`: Release-Build bestanden.
- `filament1-clang-tidy-final.log`: 36 geänderte C++-Dateien bestanden.
- `filament1-ui.pot`: neue Menü-/Dialogtexte erfolgreich extrahiert.
- Unabhängige statische Prüfung: keine weiteren Befunde.

Aktuell ausgelieferte und im GUI gestartete `runtime/OrcaSlicer.dll`:
`3bb6a4be194ebc4f1a7b4900b6bbcfddbf14a28906ed905ae976b89ef2d37f1a`.
Diese Angabe ersetzt die früheren Runtime-Hashes. Starter und Testprofil bleiben
unverändert. Live-HA wurde ausschließlich gelesen; es gab keinen Upload,
keine Bestandsänderung und keinen Druck-/Heizauftrag.

### Nachtrag Release-Qualifikation 2.5.25

Die später aktualisierte isolierte Test-DLL besitzt SHA-256
`922e0fd85dae22d7b0e19420a7d22d0a3e8943f7c5e450ec0c286818369cd81a`.
Dies ersetzt die vorherigen Runtime-Hashes. 3.090 native Registrierungen ohne
Fehler (neun Umgebungs-Skips), 90 Queue-/Dispatch-Assertions und Clang-tidy
für 52 geänderte C++-Dateien sind bestanden. Die GUI-Sperre des Mehrdruckerpfads
bei passivem Vorheizen wurde unabhängig vom HA-Modus bestätigt.
Aktueller Umfang und offene Release-Gates stehen in der
[Abschlussprüfung](RELEASE_ACCEPTANCE_2.5.25_DE.md).
