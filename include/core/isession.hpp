#pragma once

class ISession {
public:
  virtual ~ISession() = default;
  virtual void Start() = 0;
};

// ISession — minimal interface to run heterogeneous connection sessions
// (sync/async) polymorphically. Lets the runner manage a vector of sessions
// without knowing concrete types; each session starts its own execution model.
