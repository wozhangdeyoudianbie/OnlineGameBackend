CREATE TEMPORARY TABLE migration_002_precondition (
    invalid_row TINYINT UNSIGNED NOT NULL,
    CONSTRAINT check_migration_002_precondition CHECK (invalid_row = 0)
) ENGINE = InnoDB;
INSERT INTO migration_002_precondition (invalid_row)
SELECT 1
FROM result_requests
LIMIT 1;
INSERT INTO migration_002_precondition (invalid_row)
SELECT 1
FROM matches
WHERE state = 1
LIMIT 1;
DROP TEMPORARY TABLE migration_002_precondition;
ALTER TABLE players
ADD COLUMN win_count BIGINT UNSIGNED NOT NULL DEFAULT 0
AFTER player_id,
    ADD COLUMN loss_count BIGINT UNSIGNED NOT NULL DEFAULT 0
AFTER win_count;
ALTER TABLE match_players
ADD COLUMN outcome TINYINT UNSIGNED NULL
AFTER player_id,
    ADD CONSTRAINT check_match_players_outcome CHECK (
        outcome IS NULL
        OR outcome IN (1, 2)
    );
ALTER TABLE result_requests
ADD COLUMN winner_player_id BIGINT UNSIGNED NOT NULL
AFTER match_id,
    ADD COLUMN loser_player_id BIGINT UNSIGNED NOT NULL
AFTER winner_player_id,
    ADD KEY index_result_requests_winner (match_id, winner_player_id),
    ADD KEY index_result_requests_loser (match_id, loser_player_id),
    ADD CONSTRAINT check_result_request_players CHECK (winner_player_id <> loser_player_id),
    ADD CONSTRAINT check_result_request_winner FOREIGN KEY (match_id, winner_player_id) REFERENCES match_players(match_id, player_id) ON DELETE RESTRICT ON UPDATE RESTRICT,
    ADD CONSTRAINT check_result_request_loser FOREIGN KEY (match_id, loser_player_id) REFERENCES match_players(match_id, player_id) ON DELETE RESTRICT ON UPDATE RESTRICT;
