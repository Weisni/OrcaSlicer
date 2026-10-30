# Orca-Alpha: Prüfung und Integrationsplanung für Quack

Stand: 30.10.2026. Ergebnis: Die vollständige Integration der OrcaSlicer
2.5.0 Alpha ist geplant. Anwendungscode wurde dafür noch nicht integriert.

## Geprüfte Ausgangslage

Die [offizielle Alpha](https://github.com/OrcaSlicer/OrcaSlicer/releases/tag/v2.5.0-alpha)
wurde am 16.10.2026 veröffentlicht. Geprüft wurden ihre Release-Hinweise und
der festgelegte Quellstand `3880f2c58ec392ed2bd5938ab31c9c7c496c0eb2`.
Die offizielle stabile Orca-Version ist zum Prüfzeitpunkt weiterhin 2.4.2.

Quacks aktueller Remote-Hauptstand ist 2.5.24 bei
`2e38319f4e0762bc4cf44d46a28160e0fa2ae3f5`. Der lokale Hauptbranch liegt
56 Commits zurück und enthält zusätzliche, unveröffentlichte Änderungen.
Deshalb basiert die Planung auf dem Remote-Quellstand. Versionsnummern von
Quack und Orca sind keine Aussage über Funktionsgleichheit.

Seit dem gemeinsamen Vorfahren haben beide Seiten 136 identische Pfade
geändert. Dazu gehören Slicing, Materialsegmentierung, Presets, 3MF, GUI,
Druckversand und Buildsystem. Die Zahl bezeichnet gemeinsame Prüfstellen;
ein Merge wurde nicht durchgeführt und eine Konfliktzahl ist nicht ermittelt.
Die verbundene GitHub-Suche lieferte keine offenen Quack-PRs; vor Umsetzung
ist diese Momentaufnahme erneut zu prüfen.

## Neue Funktionen und ihre Auswirkungen

| Bereich | Bedeutung für Quack |
| --- | --- |
| Python-Plugins, Ereignisse und Drucker-Agenten | Neue Erweiterungen ermöglichen; doppelte Druckbuchungen und Umgehung der Vorheizprüfung verhindern |
| Parametrisches CAD und Befehlssuche | Neue Arbeitsbereiche samt Abhängigkeiten liefern; bestehende Verwaltungsseiten erreichbar halten |
| Mischfarben und neue Farbmodellimporte | Virtuelle Farben auf tatsächliche Rollen und Verbrauchsmengen auflösen |
| Texturrelief und UV-Bearbeitung | Geometrie, Bemalung, Mischfarben und eingebettete Projektbilder gemeinsam erhalten |
| Banddruck und IDEX/IQEX | Für passende Drucker verfügbar machen; korrekte Platzierung und Materialbilanz prüfen |
| Neue Düsenvarianten und Bambu-Anbindung | HA-Profile je Drucker/Düse/Flow erhalten und Druckübergabe erneut qualifizieren |
| Slicing, Vorschau, CLI, Kalibrierung und Profile | Sämtliche Änderungen des Tags übernehmen und Wechselwirkungen mit Quack prüfen |
| Laufzeit und Builds | Neues Windows-Buildverfahren, Python, CAD-Solver, OCCT und CGAL konsistent paketieren |

Die [Prüfliste](upstream/orca-2.5.0-alpha-coverage.csv) enthält alle 351
hervorgehobenen Release-Einträge mit 646 unterschiedlichen PR-Verweisen.
Sie ersetzt keine Prüfung des vollständigen Quellbaums: Weitere Korrekturen
und Profiländerungen bleiben ebenfalls Teil der vorgesehenen Integration.

## Deine Erweiterungen als verbindliche Erhaltungskriterien

Der Entwurf erfasst Rollenbestand und UUIDs, QR/NFC, Reservierungen,
Druckhistorie, Kunden/Aufträge, Archivierung, Kosten und Rechnungen;
außerdem HA-Profilautorität, lokale Profiländerungen, inkrementelle
Synchronisierung, Düsenvarianten und die auftragsgebundene Vorheizung.

Eigene Slicing-Funktionen sind ausdrücklich aufgeführt: objektbezogene
Prime Towers, projizierte Farbpenetration und Wave Overhangs. Ebenso bleiben
Korrekturen an organischen unteren Support-Interfaces, gruppierten 3MF-Importen,
Filamentzuordnungen und Projektladevorgängen Abnahmekriterien. Quack-Branding,
signierte Updates, Datenverzeichnisse und Zugangsdatenidentität gehören dazu.

Spätere lokale HA-Statuskorrekturen und Print-Again-Arbeiten müssen anhand
ihres eigenen Quell-/Installationsstands abgeglichen werden. Sie dürfen beim
Alpha-Merge weder überschrieben noch ungeprüft als bereits veröffentlicht
bezeichnet werden. Nichtplanare Untersuchungen behalten ihren Forschungsstatus.

## Empfohlene Umsetzung und offene Aufgaben

Empfohlen ist ein vollständiger Merge des festgelegten Alpha-Tags in einen
isolierten Branch auf aktuellem Quack-Stand. Einzelne Funktions-Backports
würden den Nachweis vollständiger Verfügbarkeit erheblich erschweren.

Der [englische Umsetzungsplan](superpowers/plans/2026-10-30-orca-alpha-integration.md)
beginnt mit T0: Für jede eigene Funktion wird geprüft, wie weit der Orca-Standard
sie abdecken kann. Vergleichbare Beispieldateien, Einstellungen und Testschritte
ermöglichen dir eine eigene Erprobung im unveränderten Orca. Vollständige und
teilweise Abdeckung, nötige Adapter/Plugins und verbleibende Unterschiede werden
getrennt dokumentiert. Erst nach dem Vergleich entscheidest du, ob du zum
Standard wechselst und eine eigene Integration entfallen kann. Bis dahin bleibt
ihr Verhalten erhalten. Die [Vergleichsmatrix](upstream/quack-orca-standard-comparison.md)
erfasst Ergebnisse und Entscheidungen; sie ist derzeit noch unausgefüllt.

Die folgenden neun Pakete umfassen Ausgangsstand/Merge, Builds/Abhängigkeiten,
Plugins/CAD/GUI, Materialien/Importe, Slicing, Druckübergabe, Buchführung/HA,
Profile/Übersetzungen und abschließende plattformübergreifende Qualifikation.

- [x] Release, Quellstände, Unterschiede und Quack-Erweiterungen geprüft.
- [x] Entwurf, Arbeitsplan und beide Prüflisten erstellt.
- [x] Vorhandene Root-AGENTS.md um konkrete Quack-Vorgaben ergänzt.
- [x] README und Arbeitsrichtlinien ergänzt; bisherige Inhalte erhalten.
- [x] Unabhängige Prüfung des Entwurfs: keine wesentliche Planungslücke gefunden.
- [ ] Entwurf für die spätere Code-Integration freigeben und Stände aktualisieren.
- [ ] T0-Vergleiche und Orca-Testanleitungen erarbeiten; Ersatzentscheidungen festhalten.
- [ ] Arbeitspakete T0–T9 umsetzen und Belege in den Prüflisten hinterlegen.
- [ ] Cloud-/Drucker-Agenten, Pakete und Geräteverhalten gesondert abnehmen.
- [ ] Anschließend eine Quack-Vorabversion mit Upgrade-/Rückkehrhinweisen vorbereiten.

Die Dokumentationsprüfung bestätigt 351 eindeutige Einträge, 646 PR-Verweise
und 136 gemeinsame Pfade. SHA-256-Vergleiche bestätigen unveränderte Inhalte
der zwölf zuvor erfassten übrigen Dateien sowie den vollständigen Erhalt der
bisherigen Bytes von AGENTS.md, README und Arbeitsrichtlinien. Alle lokalen
Links in Agentenanweisungen, Entwurf, Plan und Bericht sind geprüft und gültig.
Es gibt für diesen Entwurf keine neuen Build-,
GUI-, CI-, Cloud- oder Hardware-Ergebnisse; deren Prüfungen stehen im Plan.
