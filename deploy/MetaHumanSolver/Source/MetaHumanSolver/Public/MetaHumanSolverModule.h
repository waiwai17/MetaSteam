// MetaHumanSolverModule.h
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMetaHumanSolverModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenu();
    TSharedRef<class SDockTab> SpawnToolTab(const class FSpawnTabArgs& Args);
};
