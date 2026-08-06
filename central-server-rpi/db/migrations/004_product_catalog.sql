ALTER TABLE product ADD COLUMN product_id TEXT;

CREATE TABLE product_catalog (
    barcode TEXT PRIMARY KEY NOT NULL
        CHECK(length(barcode) = 13 AND barcode NOT GLOB '*[^0-9]*'),
    product_id TEXT NOT NULL UNIQUE CHECK(length(product_id) > 0),
    product_name TEXT NOT NULL CHECK(length(product_name) > 0),
    destination TEXT NOT NULL CHECK(length(destination) > 0),
    active INTEGER NOT NULL DEFAULT 1 CHECK(active IN (0, 1)),
    created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),
    updated_at_ms INTEGER NOT NULL CHECK(updated_at_ms >= created_at_ms)
);

CREATE INDEX idx_product_catalog_destination
    ON product_catalog(destination) WHERE active = 1;

INSERT INTO product_catalog(
    barcode,
    product_id,
    product_name,
    destination,
    active,
    created_at_ms,
    updated_at_ms
)
VALUES(
    '5901234123457',
    'VEDA107',
    'VEDA107 기본 상품',
    '1',
    1,
    CAST(strftime('%s', 'now') AS INTEGER) * 1000,
    CAST(strftime('%s', 'now') AS INTEGER) * 1000
);
