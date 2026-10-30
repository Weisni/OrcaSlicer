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

## Korrektur der HA-Adresse (30.10.2026)

Die erste Datenkopie ließ auch die nicht geheime HA-Adresse weg. Deshalb zeigte
der Dialog die lokale Demo-Adresse `http://127.0.0.1:8765/api/quack_material_demo/materials`.
Ein anderer lokaler Dienst antwortet dort mit 404. Die korrigierte und im laufenden
Testprofil gespeicherte Adresse ist
`http://homeassistant.local:8123/api/quack_material_demo/materials`.
HA ist unter dieser Adresse erreichbar und antwortet ohne Token mit 401.
Die Snapshot-Erstellung behält künftig sichere Endpunktadressen bei; Zugangsdaten
in URLs, Query-Parameter und Fragmente werden nicht übernommen. Zwei Regressionstests
bestanden nach vorher nachgewiesenem Auslassen der gültigen Adresse.

Die Testverbindung bleibt bis zur ausdrücklichen Anmeldung deaktiviert. Im HA-Dialog
„Use Home Assistant instead of printer material labels“ aktivieren, den HA-Token
im Passwortfeld eintragen und mit OK verbinden. Der Token gehört ausschließlich
in diesen Dialog. Authentifizierter Bestandsabruf ist noch nicht bestätigt.

## Korrektur bestehender HA-Materialprofile (30.10.2026)

Die Meldung über ein abweichendes Einstellungsschema entstand beim Einlesen
bisheriger HA-Profile: Quack 2.5.24 liefert 149 Einstellungen, die Beta erwartet
151. Bei 24 lokal vorhandenen HA-Profilkopien fehlen ausschließlich
`filament_retract_length_toolchange` und
`filament_retract_restart_extra_toolchange`.

Der Leser und der Cache akzeptieren jetzt zusätzlich genau dieses bisherige
Schema. Fehlende neue Rückzugseinstellungen erhalten `nil` und erben damit die
Druckereinstellungen. Originalinhalt, SHA-256 und unveränderte Profilidentitäten
bleiben erhalten. Explizite neue Overrides werden weiterhin übertragen; andere
fehlende oder unbekannte Einstellungen bleiben Fehler. Eine erneute Veröffentlichung
aller Profile oder eine Änderung der installierten HA-Integration ist nicht nötig.

Nachgewiesen sind zunächst fehlschlagende Decode-/Cache-Regressionen, anschließend
25 bestandene Materialtests mit 299 Prüfungen. Der vollständige native Lauf hat
3.047 registrierte Tests, null Fehler und neun umgebungsbedingte Überspringungen.
Das unabhängige Quellreview fand keine blockierenden Befunde. Protokolle liegen
unter `build-test/evidence/ha-schema-*.log`.

Der Release-Build wurde in das lokale Testpaket übernommen und gestartet.
Die DLL-Prüfsumme lautet
`B79FE87E5D3A8413C676C138AD21F2E1EDF35DACA3E2B6504C4EDE61DD83C422`.
Das offene Projekt wurde vorher als
`build-test/comparison/ha-schema-project-backup.3mf` gesichert und wieder geöffnet.
Der Live-Bestandsabruf bleibt offen: Nach dem Neustart fehlt der nur für die
Sitzung eingegebene Token; HA antwortet mit HTTP 401. Den Token erneut direkt im
HA-Dialog eingeben. „Remember“ speichert ihn bei Bedarf im getrennten
Zugangsspeicher der Testversion. Es wurden keine HA-Profile überschrieben und
keine Druck-/Heizbefehle ausgelöst.

### Berichtigung zur Token-Persistenz

Die vorherige Vermutung eines Sitzungstokens war falsch. Der Windows-Zugangsspeicher
enthält den HA-Eintrag für den Testprofilpfad mit Vorwärtsschrägstrichen. Der
manuelle Neustart verwendete denselben Ordner mit Rückwärtsschrägstrichen.
`HaInventoryCredentials::service` hasht den unveränderten Datenpfad mit dem
Serverursprung; beide Schreibweisen erzeugen unterschiedliche Schlüssel.
Der ursprüngliche Token wurde somit gespeichert, beim Neustart aber unter einem
anderen Schlüssel gesucht. Geprüft wurden nur Eintragsnamen, keine Tokenwerte.
Der lokale Starter normalisiert nun den absoluten Testprofilpfad auf die bereits
verwendete Schreibweise mit `/`. Eine allgemeine Normalisierung im Anwendungscode
benötigt zusätzlich eine Migration bestehender Zugangsschlüssel und bleibt offen.

## Korrektur des HA-Bestandsimports (30.10.2026)

Ein authentifizierter, ausschließlich lesender Abruf mit dem gespeicherten
Testzugang zeigt 33 Rollen und 459 Verbrauchszuordnungen. Bei genau einer
Zuordnung fehlen `actual_weight_mg` und `actual_material_cost_micros`.
Eine SQLite-Kopie im Arbeitsspeicher ergänzt diese Felder mit `NULL`;
alle anderen gelieferten Felder bleiben unverändert. Der strenge Graphvergleich
verweigerte deshalb den Import mit „HA inventory graph changed during import“.

Der Import und die kanonische Integritätsdarstellung vereinheitlichen jetzt
ausschließlich diese beiden optionalen Felder. Fehlend bedeutet unbekannt,
nicht null Gramm, kostenlose Nutzung oder eine alte Messung aus dem lokalen
Cache. Explizite Messwerte, Rollenzuordnungen, Bestands- und Reservierungsprüfungen
sowie der atomare Abbruch bei echten Graphabweichungen bleiben erhalten.

Der neue Regressionstest löste vor der Korrektur exakt die gemeldete Ausnahme
aus. Danach bestanden 19 Import-/Transporttests mit 102 Prüfungen, einschließlich
Rücknahme veralteter Messwerte und Ablehnung anderer unvollständiger Datensätze.
Protokolle: `build-test/evidence/ha-graph-*.log`. Es wurden keine Live-Daten auf HA
verändert und keine Druck- oder Heizbefehle gesendet.

Im folgenden GUI-Test trat eine weitere falsche Änderungsmeldung für bereits
gespeicherte HA-Profile auf. Der allgemeine Orca-Lader erweitert Broadcastwerte
auf Standard-/High-Flow-Varianten und verändert damit deren Inhaltsidentität.
Bei verwalteten HA-Root-Filamentprofilen bleiben die gespeicherten Vektorlängen
jetzt genauso erhalten wie beim ersten Download. Normale Profile behalten ihre
bisherige Variantenbehandlung. Der Reload-Test reproduzierte die Abweichung
vorher sowohl für das aktuelle als auch für das bisherige HA-Schema.

Am endgültigen Stand bestanden 44 gezielte Tests mit 406 Prüfungen. Ein
unabhängiges Quellreview fand keine blockierenden oder wichtigen Befunde und
prüfte auch den Broadcast-Fallback der nachgelagerten Variantenauflösung.
Die erneut gebaute und übernommene DLL hat SHA-256
`7F5CD0A9B69B24DA43690D239E0FA76D09811C00EC2DA44192C38BFE8477BFAA`.

Nach Neustart wurde der gespeicherte Token automatisch geladen. Die GUI zeigt
die sechs belegten Plätze A1–A4, HT1 und EXT mit lesbaren Namen und Beständen.
Der Synchronisationsdialog öffnet sich mit diesen sechs Rollen ohne Graph- oder
Profilkonfliktmeldung. Er bleibt zur Auswahl des Benutzers offen; keine neue
Rollenzuordnung oder Veröffentlichung nach HA wurde bestätigt. Das Projekt
mit seinen bisherigen Einstellungen wurde zuvor zusätzlich als
`build-test/comparison/ha-graph-project-backup.3mf` gesichert und wieder geöffnet.

Der vollständige native Abschlusslauf nach beiden Korrekturen umfasst 3.049
registrierte Tests: null Fehler, neun umgebungsbedingt übersprungen.
Beleg: `build-test/evidence/ha-import-profile-ctest-final.log`.

## Erweiterte HA-Abnahme und einheitliche Flow-Auswahl

Der nachfolgende Stand ersetzt die oben genannten DLL-Hashes und Testzahlen.
Die zusätzliche Quack-Flow-Auswahl im Variant-Kasten wurde durch die native
Beta-Nozzle-Auswahl ersetzt. Geprüft wurden Rollenwahl, gemeinsamer Import,
Profilwechsel bei 0,4/0,6 mm und Standard/High Flow, expliziter Profilupload mit
Konflikt und Antwortverlust sowie Neustart und Offline-Anzeige. Der gespeicherte
Token funktioniert auch mit anderer Schreibweise desselben Testdatenpfads.

Die vollständigen Abläufe, Korrekturen, Auslieferungsnachweise und verbleibenden
Live-/Hardwaregrenzen stehen im [HA-Abnahmebericht](HA_BETA_ACCEPTANCE_2026-10-30_DE.md).
Die Benutzerprojektkopie `build-test/comparison/ha-full-audit-user-backup.3mf`
enthält den gesicherten Zustand vor den synthetischen Tests.

## Nachtrag: Mixed-Filament beim Düsenwechsel

Beim Wechsel mit `remember_printer_config` blieben die Projektplätze und Farben
erhalten, die sieben Mischfelder wurden jedoch aus dem Ziel-Druckersnapshot
neu geladen. Fehlende oder ältere Definitionen machten die virtuelle Mischung
dadurch zu einem normalen Filament. Die Projektdefinition einschließlich
Komponenten, Anteilen und Farbverlauf bleibt jetzt zusammen mit ihren Indizes
erhalten; zusätzliche Plätze bekommen leere beziehungsweise false-Werte.
Normales Laden eines eigenen Druckersnapshots bleibt unverändert.

Der native Regressionstest reproduzierte 66 fehlerhafte Prüfungen vor der
Korrektur. Danach bestanden 28 Mixer-/Farbtests mit 673 Prüfungen, die komplette
Suite mit 3.064 registrierten Tests ohne Fehler und neun Umgebungsskips sowie
clang-tidy für 28 geänderte C++-Dateien. Ein unabhängiges Quellreview fand keine
weiteren Befunde. Belege: `build-test/evidence/mixed-nozzle-*.log`.

Die GUI-Kette 0,2 → 0,4 → 0,6 → 0,2 mm behielt Material 8 als 90/10-Mischung aus
Material 1 und 2. Auch der widersprechende gespeicherte 0,6-mm-Mix ersetzte diese
aktive Projektdefinition nicht. Die gespeicherte 3MF wurde anschließend direkt
auf Kennzeichen, Komponenten und Anteile geprüft. Kein Druck/Heizen/HA-Upload.

Der unmittelbar vor dieser Korrektur aktive Benutzerzustand ist separat unter
`build-test/comparison/mixed-nozzle-user-state-before-fix.3mf` gesichert.
Die noch vorhandenen Druckersnapshots ermöglichten zwei getrennte Wiederherstellungen:

- `mixed-nozzle-recovered-90-10.3mf`: Mischung aus dem 0,4-mm-Snapshot, auf Platz 8.
  Diese Kopie wurde im GUI getestet, gespeichert und ist geöffnet.
- `mixed-nozzle-recovered-67-33.3mf`: alternative Mischung aus dem 0,6-mm-Snapshot,
  auf Platz 7. Separat wiederhergestellt und als ZIP/Projektinhalt geprüft.

Beide Dateien liegen unter `build-test/comparison`; die gesicherte Originalkopie
bleibt erhalten. Die Wiederherstellung änderte ausschließlich die Mischfelder,
keine Modellgeometrie oder HA-Datenbank.

Aktuell ausgelieferte `OrcaSlicer.dll`, welche die vorherigen DLL-Hashes ersetzt:
`a8312dcceb7bcb1b1895a116e68aa5ed189e003f33333df216164547e88cf4b2`.
Der unveränderte Starter ist `build-test/QuackSlicer-Beta-Test/Start-Quack-Beta-Test.cmd`.

### Nachtrag A3-Farbe

Der neuere [A3-Abnahmenachtrag](HA_BETA_ACCEPTANCE_2026-10-30_DE.md#nachtrag-a3-farbe-und-virtuelle-materialplätze)
ersetzt den vorherigen Runtime-Hash und Teststand: 3.077 native Registrierungen,
null Fehler, neun Umgebungs-Skips; Release-DLL SHA-256
`7203b7b7ad17a0b0c5106c9f3444f1e16fe7eec89fe0135d9972f82e4e726058`.
Geöffnet ist nun die getrennte `build-test/comparison/a3-color-repaired.3mf`
mit A3 auf Platz 2 und der wiederhergestellten 50/50-Mischung auf Platz 3.
Farbauswahl und Neustart wurden im GUI geprüft. Verbleibende Profilabweichungen
und die unberührten Originalbindungen sind im Abnahmenachtrag ausgewiesen.

### Nachtrag Profilgleichheit nach Neustart

Die A3-Profilabweichung ist inzwischen behoben und im GUI nach Synchronisation,
Speichern und vollständigem Neustart geprüft. Geöffnet ist die neue separate
`build-test/comparison/a3-profile-repaired.3mf`. Der Starter bleibt unverändert.
Aktuelle Release-DLL SHA-256:
`cb7a9b6a6c81984e7e77053d3a4462b00fa978ac7380b4211b10662c3ed0989d`.
3.078 native Registrierungen ohne Fehler (neun Umgebungs-Skips), 79 gezielte
Fälle mit 1.241 Assertions und clang-tidy für 35 geänderte Dateien bestanden.
Ursache, Schutz echter lokaler Änderungen, Backups und der getrennte bestehende
Filament-1-Eintrag stehen im [Profil-Nachtrag](HA_BETA_ACCEPTANCE_2026-10-30_DE.md#nachtrag-profilgleichheit-nach-neustart).

### Nachtrag A4 und virtuelle Mischungsanzeige

Die ausdrückliche Benutzerauswahl A4 für Filament 1 ist erhalten. Geöffnet ist
`build-test/comparison/a4-a3-mixed-test.3mf` mit A4, A3 und einer ungebundenen
virtuellen 50/50-Mischung auf Platz 3. Nach Neustart zeigt der HA-Vergleich keine
Profil-/Farbabweichungen und keinen Erstprofil-Hinweis. Im Objekt-Kontextmenü
heißt die dritte Auswahl jetzt „Mixed Filament 3“.

Aktuelle Release-DLL SHA-256:
`3bb6a4be194ebc4f1a7b4900b6bbcfddbf14a28906ed905ae976b89ef2d37f1a`.
3.079 native Registrierungen ohne Fehler (neun Umgebungs-Skips), 80 gezielte
Fälle mit 1.244 Assertions, clang-tidy für 36 geänderte Dateien sowie Extraktion
der neuen UI-Texte bestanden. Details im [aktuellen Abnahmenachtrag](HA_BETA_ACCEPTANCE_2026-10-30_DE.md#nachtrag-a4-zuordnung-und-mischungsbezeichnung).
