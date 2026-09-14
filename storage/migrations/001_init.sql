CREATE TABLE players (
    player_id BIGINT UNSIGNED NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (player_id)
) ENGINE = InnoDB;

CREATE TABLE matches (
    match_id VARBINARY(64) NOT NULL,
    create_request_id VARBINARY(64) NOT NULL,
    state TINYINT UNSIGNED NOT NULL DEFAULT 0,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    completed_at DATETIME(6) NULL,
    PRIMARY KEY (match_id),
    CONSTRAINT check_create_request_id UNIQUE (create_request_id),
    CONSTRAINT check_matches_state CHECK (state IN (0, 1))
) ENGINE = InnoDB;

CREATE TABLE match_players(
    match_id VARBINARY(64) NOT NULL,
    player_id BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (match_id,player_id),
    KEY index_match_players(player_id,match_id),
    CONSTRAINT check_match_in_matches
        FOREIGN KEY (match_id)
        REFERENCES matches(match_id)
        ON DELETE RESTRICT
        ON UPDATE RESTRICT,
    CONSTRAINT check_player_in_match
        FOREIGN KEY (player_id)
        REFERENCES players (player_id)
        ON DELETE RESTRICT
        ON UPDATE RESTRICT
)ENGINE = InnoDB;

CREATE TABLE active_assignments(
    match_id VARBINARY(64) NOT NULL,
    player_id BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (player_id),
    KEY index_active_assignments_match_players(match_id,player_id),
    CONSTRAINT check_active_assignment
        FOREIGN KEY (match_id,player_id)
        REFERENCES match_players(match_id,player_id)
        ON DELETE RESTRICT
        ON UPDATE RESTRICT
)ENGINE = InnoDB;

CREATE TABLE result_requests(
    request_id VARBINARY(64) NOT NULL,
    match_id VARBINARY(64) NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (request_id),
    KEY index_result_requests_match(match_id),
    CONSTRAINT check_result_request_match
        FOREIGN KEY (match_id)
        REFERENCES matches(match_id)
        ON DELETE RESTRICT
        ON UPDATE RESTRICT
)ENGINE=InnoDB;
