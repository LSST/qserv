CREATE TABLE IF NOT EXISTS `chunkMap` (
  `worker` VARCHAR(256) NOT NULL COMMENT 'A unique identifier of a worker hosting the chunk replica',
  `database` VARCHAR(256) NOT NULL COMMENT 'The name of a database',
  `table` VARCHAR(256) NOT NULL COMMENT 'The name of a table',
  `chunk` INT UNSIGNED NOT NULL COMMENT 'The number of a chunk',
  `size` BIGINT UNSIGNED NOT NULL COMMENT 'The size of a chunk')
ENGINE = InnoDB
COMMENT = 'Chunk disposition across workers';

CREATE TABLE IF NOT EXISTS `chunkMapStatus` (
  `update_time` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT 'The most recent update time of the map')
ENGINE = InnoDB
COMMENT = 'Satus info on the chunk map';
