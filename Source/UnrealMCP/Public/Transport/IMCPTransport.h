#pragma once

#include "CoreMinimal.h"

class FMCPServer;

class IMCPTransport
{
public:
    virtual ~IMCPTransport() = default;

    virtual FString GetTransportName() const = 0;
    virtual bool Start(FMCPServer& Server) = 0;
    virtual void Stop() = 0;
};
