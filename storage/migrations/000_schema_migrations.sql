CREATE TABLE IF NOT EXISTS schema_migrations (
    version INT UNSIGNED NOT NULL,
    name VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    checksum CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    applied_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (version),
    CONSTRAINT check_schema_migrations_name UNIQUE (name)
) ENGINE = InnoDB;
