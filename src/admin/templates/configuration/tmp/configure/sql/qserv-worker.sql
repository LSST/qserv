CREATE USER IF NOT EXISTS '{{MYSQLD_USER_QSERV}}'@'localhost';
CREATE USER IF NOT EXISTS '{{MYSQLD_USER_QSERV}}'@'%';

-- Used by xrootd Qserv plugin:
-- to publish LSST databases and chunks

DROP DATABASE IF EXISTS qservw_worker;
CREATE DATABASE qservw_worker;

GRANT ALL ON qservw_worker.* TO '{{MYSQLD_USER_QSERV}}'@'localhost';
GRANT ALL ON qservw_worker.* TO '{{MYSQLD_USER_QSERV}}'@'%';

CREATE TABLE qservw_worker.Dbs (

  `db` CHAR(200) NOT NULL,

  PRIMARY KEY (`db`)

) ENGINE=InnoDB;

CREATE TABLE qservw_worker.Chunks (

  `db`    CHAR(200)    NOT NULL,
  `chunk` INT UNSIGNED NOT NULL,

  UNIQUE KEY(`db`,`chunk`)

) ENGINE=InnoDB;

CREATE TABLE qservw_worker.Id (

  `id`      VARCHAR(64)  NOT NULL,
  `type`    ENUM('UUID') DEFAULT 'UUID',
  `created` TIMESTAMP    DEFAULT CURRENT_TIMESTAMP,

  UNIQUE KEY (`type`)

) ENGINE=InnoDB;

INSERT INTO qservw_worker.Id (`id`) VALUES (UUID());

CREATE TABLE IF NOT EXISTS qservw_worker.QMetadata (

  `metakey` CHAR(64) NOT NULL COMMENT 'Key string',
  `value`   TEXT         NULL COMMENT 'Value string',

  PRIMARY KEY (`metakey`)

) ENGINE = InnoDB COMMENT = 'Metadata about database as a whole, key-value pairs';

INSERT INTO qservw_worker.QMetadata (`metakey`, `value`) VALUES ('version', '2');


GRANT ALL ON `q\_memoryLockDb`.* TO '{{MYSQLD_USER_QSERV}}'@'localhost';
GRANT ALL ON `q\_memoryLockDb`.* TO '{{MYSQLD_USER_QSERV}}'@'%';

-- Subchunks databases
GRANT ALL ON `Subchunks\_%`.* TO '{{MYSQLD_USER_QSERV}}'@'localhost';
GRANT ALL ON `Subchunks\_%`.* TO '{{MYSQLD_USER_QSERV}}'@'%';


-- Create user for external monitoring applications
CREATE USER IF NOT EXISTS '{{MYSQLD_USER_MONITOR}}'@'localhost' IDENTIFIED BY '{{MYSQLD_PASSWORD_MONITOR}}';
CREATE USER IF NOT EXISTS '{{MYSQLD_USER_MONITOR}}'@'%' IDENTIFIED BY '{{MYSQLD_PASSWORD_MONITOR}}';
GRANT PROCESS ON *.* TO '{{MYSQLD_USER_MONITOR}}'@'localhost';
GRANT PROCESS ON *.* TO '{{MYSQLD_USER_MONITOR}}'@'%';
