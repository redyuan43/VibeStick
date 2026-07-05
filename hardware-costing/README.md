# VibeStick Hardware Costing

Costing model for the two currently supported hardware targets:

- M5Stack StickS3
- M5StickC Plus

Primary workbook:

- `bom-costing.xlsx`

Costing basis:

- Scope: self-developed production BOM, not off-the-shelf M5Stack finished-device purchasing.
- Quantity: 10,000 units.
- Region: domestic China-oriented sourcing, with public distributor prices used where available.
- Currency: CNY summary. USD public prices are converted through the editable FX assumption in the workbook.

Important caveats:

- The official schematic PDFs do not provide a clean manufacturer production BOM export, so this first workbook combines schematic-identified major parts, official product specs, grouped passives/discretes, and RFQ placeholders.
- Yellow/low-confidence rows must be replaced with supplier quotes before PO-level cost decisions.
- The Summary sheet intentionally separates high-confidence public-price rows from RFQ/estimate exposure.

Main reference bundle:

- `../hardware-reference-downloads/`

Useful next step:

- Request exact BOM/AVL/Gerbers from M5Stack or rebuild the schematic in EDA and export a reference-designator BOM, then replace grouped rows with exact line items.
