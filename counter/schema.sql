-- one row per project, module and day: how many times it was added. No address, no agent, nothing else.
CREATE TABLE IF NOT EXISTS adds (
  project TEXT NOT NULL,
  module  TEXT NOT NULL,
  day     TEXT NOT NULL,
  n       INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (project, module, day)
);
