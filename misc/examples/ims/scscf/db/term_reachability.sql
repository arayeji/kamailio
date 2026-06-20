-- =====================================================================
-- Terminating VoLTE — UE Reachability & IMS Restoration : schema
-- ---------------------------------------------------------------------
-- OPTIONAL. The live paging state is held in the in-memory `htable`
-- (term_pending) for lowest latency. These tables add:
--   * multi-instance persistence / audit of suspended sessions
--   * per-subscriber / per-realm CS fallback + unreachable policy
--   * a short-TTL cache of the last HSS reachability answer
--
-- Engine InnoDB for row-level locking (concurrent terminating calls).
-- 3GPP refs: TS 23.237 (T-ADS), TS 23.272 (CSFB), TS 29.272 (S6a).
-- =====================================================================

-- ---------------------------------------------------------------------
-- Audit / persistence of suspended terminating sessions (Requirement 8/10).
-- One row per terminating INVITE that entered the reachability procedure.
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `term_reach_pending` (
  `id`            INT(10) UNSIGNED NOT NULL AUTO_INCREMENT,
  `impu`          VARCHAR(255)     NOT NULL,
  `impi`          VARCHAR(255)             DEFAULT NULL,
  `call_id`       VARCHAR(255)     NOT NULL,
  `t_index`       INT(10) UNSIGNED         DEFAULT NULL,  -- tm suspend handle
  `t_label`       INT(10) UNSIGNED         DEFAULT NULL,
  `serving_mme`   VARCHAR(255)             DEFAULT NULL,
  `vlr_number`    VARCHAR(64)              DEFAULT NULL,
  `rat_type`      VARCHAR(16)              DEFAULT NULL,
  `ue_reachable`  TINYINT(1)       NOT NULL DEFAULT 0,
  `attempts`      INT(10) UNSIGNED NOT NULL DEFAULT 0,
  `state`         ENUM('paging','restored','timeout','cs','unreachable','failed')
                                   NOT NULL DEFAULT 'paging',
  `created_at`    DATETIME         NOT NULL,
  `deadline_at`   DATETIME         NOT NULL,
  `resolved_at`   DATETIME                 DEFAULT NULL,
  `final_dest`    VARCHAR(255)             DEFAULT NULL,  -- ims-contact | cs:<vlr> | vm | reject:<code>
  PRIMARY KEY (`id`),
  KEY `k_impu`       (`impu`),
  KEY `k_call_id`    (`call_id`),
  KEY `k_state`      (`state`),
  KEY `k_deadline`   (`deadline_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------
-- Per-subscriber / per-realm policy (Requirement 6,7,8).
-- Lookup precedence: exact impu  >  realm wildcard ('@realm')  >  '*'.
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `term_reach_policy` (
  `id`                    INT(10) UNSIGNED NOT NULL AUTO_INCREMENT,
  `match_key`             VARCHAR(255)     NOT NULL,      -- impu | '@realm' | '*'
  `paging_timeout_s`      INT(10) UNSIGNED NOT NULL DEFAULT 5,
  `max_paging_attempts`   INT(10) UNSIGNED NOT NULL DEFAULT 2,
  `ue_up_wait_s`          INT(10) UNSIGNED NOT NULL DEFAULT 2,
  `cs_enabled`            TINYINT(1)       NOT NULL DEFAULT 1,
  `cs_route_template`     VARCHAR(255)             DEFAULT 'sip:{user}@{vlr}',
  `unreachable_action`    ENUM('voicemail','forward','reject')
                                           NOT NULL DEFAULT 'voicemail',
  `unreachable_target`    VARCHAR(255)             DEFAULT NULL, -- VM/forward URI
  `unreachable_code`      SMALLINT(5) UNSIGNED     DEFAULT 480,
  `updated_at`            DATETIME         NOT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `u_match_key` (`match_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------
-- Short-TTL cache of last HSS reachability answer (Requirement 5,10).
-- Used as conservative fallback when the HSS is momentarily unavailable.
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `term_reach_hss_cache` (
  `impu`          VARCHAR(255)     NOT NULL,
  `ims_state`     VARCHAR(32)              DEFAULT NULL,
  `serving_mme`   VARCHAR(255)             DEFAULT NULL,
  `vlr_number`    VARCHAR(64)              DEFAULT NULL,
  `rat_type`      VARCHAR(16)              DEFAULT NULL,
  `ue_reachable`  TINYINT(1)       NOT NULL DEFAULT 0,
  `cached_at`     DATETIME         NOT NULL,
  `expires_at`    DATETIME         NOT NULL,
  PRIMARY KEY (`impu`),
  KEY `k_expires` (`expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Default global policy row.
INSERT INTO `term_reach_policy`
  (`match_key`,`paging_timeout_s`,`max_paging_attempts`,`ue_up_wait_s`,
   `cs_enabled`,`cs_route_template`,`unreachable_action`,`unreachable_target`,
   `unreachable_code`,`updated_at`)
VALUES
  ('*',5,2,2,1,'sip:{user}@{vlr}','voicemail',
   'sip:voicemail@ims.mnc001.mcc001.3gppnetwork.org',480,NOW())
ON DUPLICATE KEY UPDATE `updated_at`=NOW();
