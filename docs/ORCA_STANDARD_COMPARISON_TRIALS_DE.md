
# Orca-Standard und Quack vergleichen

Diese Anleitung gehört zur lokalen Beta-Integration. Sie beschreibt die noch
ausstehenden Nutzertests; aus der Erstellung der Modelle folgt keine Gleichwertigkeit.

## Vorbereitung

Verwende die offizielle OrcaSlicer 2.5.0 Beta und „QuackSlicer Beta Test“ mit
getrennten Datenverzeichnissen. Die synthetischen Dateien werden mit
`python tools/orca-integration/create_comparison_fixtures.py --output <Ordner>`
erstellt und liegen auch im lokalen Vergleichspaket. Drucker-, Filament- und
Prozesseinstellungen müssen für jeden Vergleich übereinstimmen. Halte abweichende
Einstellungen und verwendete Versionen fest. Die Tests benötigen nur Import,
Slicing, Vorschau und Speichern; ein Druckstart ist dafür nicht erforderlich.

## 1. Farbpenetration: Standardtiefe gegenüber eigenen Modi

1. Öffne `paint-depth-coupon.stl` in Quack und wähle zwei passende PLA-Filamente.
2. Verwende 0,2 mm Schichthöhe und vier obere/untere Schalenlagen. Bemale einen
   kleinen zusammenhängenden Bereich auf der Oberseite mit der zweiten Farbe.
3. Speichere dieses bemalte Projekt als Vergleichskopie. Öffne dieselbe Kopie
   auch in Orca und protokolliere Importwarnungen/ersetzte unbekannte Optionen.
4. Vergleiche zunächst den normalen Modus: `color_penetration_mode=inwards`
   in Quack, `mmu_surface_normal_penetration_depth=0`; in beiden Anwendungen
   `top_color_penetration_layers` nacheinander 0, 2 und 6. Die Beta hat diese
   oberen und unteren Tiefeneinstellungen als Standard. Vergleiche Zahl und
   Fläche der farbigen Lagen, Infill, Filamentwechsel und Verbrauch.
5. Wiederhole mit bemalter Unterseite und `bottom_color_penetration_layers`.
6. Vergleiche separat Quacks `projected` mit dem Orca-Ergebnis. Orca bietet
   diesen Modusschalter nicht. Prüfe, ob dir dessen Standardergebnis für den
   konkreten Anwendungsfall trotzdem genügt.
7. Prüfe Quacks zusätzliche Tiefe entlang der Flächennormalen separat auf einer
   schrägen bemalten Fläche. Eine vertikale Lagenzahl ist kein Beleg für dasselbe
   Ergebnis wie `mmu_surface_normal_penetration_depth`.

Entscheidung: Standard-Lagentiefe übernehmen, Zusatzmodus behalten, oder einen
dokumentierten Unterschied akzeptieren. Gleiche Lagenzahlen allein beweisen
keine gleiche Farbverteilung.

## 2. Objektbezogene Prime Towers

1. Importiere `two-colored-objects.3mf` als zwei getrennte Objekte und prüfe die
   grüne/violette Zuordnung. Bemale auf jedem Objekt zusätzlich einen Bereich
   mit der jeweils anderen Farbe und speichere diese gemeinsame Eingabekopie.
2. Wähle in beiden Programmen denselben Drucker und Druckreihenfolge nach Objekt.
3. Prüfe in Orca, welche Tower-Kombination tatsächlich verfügbar ist und wo
   Materialwechsel, Spülung und Tower im erzeugten G-Code liegen.
4. Aktiviere in Quack `enable_prime_tower_by_object`; prüfe die getrennten
   Positionen, Speichern/Neuöffnen sowie Vorschau und Spülverbrauch je Objekt.
5. Halte fest, ob die Standardfunktion deinen tatsächlichen Bedarf abdeckt oder
   ob getrennte Towers pro Objekt weiterhin benötigt werden.

## 3. Organische Supports und vorhandene Fehlerkorrekturen

1. Slice `support-capital.stl` mit organischen Baumstützen und erlaubtem Kontakt
   auf dem Modell. Verwende gleiche Materialien, drei untere/obere Interface-
   lagen sowie 0,2 mm Objekt-Lagenhöhe.
2. Vergleiche zunächst obere Z-Distanz 0 und untere Z-Distanz 0,2 mm; danach
   weitere identische Abstandskombinationen.
3. Prüfe untere Kontaktflächen, Interface-Material, wirklichen Abstand und
   Überschneidungen mit dem Modell in der Vorschau.
4. Halte Einschränkungen der unabhängigen Stützhöhe fest. Eine ausgeblendete
   oder deaktivierte Standardoption ist keine bestätigte Ersatzfunktion.

## 4. Wave Overhangs und normale Brücken

Slice `overhang-coupon.stl` zunächst in beiden Programmen mit normalen
Überhängen/Brücken. Vergleiche Quack danach mit aktivierten Wave Overhangs:
Verankerung, Pfade, verbleibende Stützen, Decklagen, Vorschau und Material.
Die Standard-Beta hat keine Wave-Overhangs-Option. Entscheide anhand des
benötigten Ergebnisses, ob dir normale Brücken/Supports für diese Anwendung
genügen. Digitale Unterschiede ersetzen keine spätere Druckqualitätsprüfung.

## 5. Import und Profilwechsel

Öffne `two-colored-objects.3mf` jeweils als neue Kopie und durch Ersetzen in
einem bestehenden Projekt. Prüfe Objekt-/Volumen-/Farbzuordnung, Gruppierung
und die Auswahl „Projekt-Drucker laden“ beziehungsweise „aktuellen Drucker
behalten“. Wiederhole mit einer Kopie eines repräsentativen eigenen Projekts.
Notiere, welche Wahlmöglichkeiten der jeweilige Standard tatsächlich bietet.

## Ergebnisbogen

Für jeden Teiltest festhalten:

- Anwendung, exakte Version, Modell/Projekt und gewählte Einstellungen;
- beobachtetes Ergebnis, Screenshot/G-Code und Unterschiede;
- reicht der Orca-Standard für deinen Anwendungsfall: ja / teilweise / nein;
- gewünschte Entscheidung: Standard verwenden / kleiner Adapter / Custom behalten;
- akzeptierte Einschränkungen und noch fehlende Prüfungen.

Bestätigte Entscheidungen werden in
[der Vergleichsmatrix](upstream/quack-orca-standard-comparison.md) dokumentiert.
Eigene Funktionen bleiben bis zu dieser Entscheidung erhalten.
