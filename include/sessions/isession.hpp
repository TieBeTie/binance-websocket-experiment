#pragma once

class ISession {
public:
  virtual ~ISession() = default;
  virtual void Start() = 0;
};
