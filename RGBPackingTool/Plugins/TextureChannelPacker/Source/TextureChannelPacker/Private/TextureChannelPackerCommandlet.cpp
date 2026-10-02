#include "TextureChannelPackerCommandlet.h"
#include "TextureChannelPackerCore.h"
#include "TextureChannelPackerShared.h"
#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    constexpr int32 ExitSuccess = 0;
    constexpr int32 ExitUsageError = 1;
    constexpr int32 ExitJobsFailed = 2;

    /** Version of the result file layout. Bump on breaking changes. */
    constexpr int32 ResultSchemaVersion = 1;

    const TCHAR* const GSlotLetters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

    /** Resolves a command-line path against the launch directory (the engine changes the working directory at startup). */
    FString ResolveCommandLinePath(const FString& Path)
    {
        return FPaths::IsRelative(Path) ? FPaths::ConvertRelativePathToFull(FPaths::LaunchDir(), Path) : Path;
    }

    /**
     * Accepts "/Game/Folder/T_Name", "/Game/Folder/T_Name.T_Name" and "Texture2D'/Game/Folder/T_Name.T_Name'"
     * and returns the object path ("/Game/Folder/T_Name.T_Name").
     */
    FString NormalizeAssetPath(const FString& InPath)
    {
        const FString TrimmedPath = InPath.TrimStartAndEnd();
        FString Path = FPackageName::ExportTextPathToObjectPath(TrimmedPath);
        if (!Path.IsEmpty() && !Path.Contains(TEXT(".")))
        {
            Path += TEXT(".") + FPackageName::GetShortName(Path);
        }
        return Path;
    }

    /** Fails on keys the schema does not define, so a typo surfaces as an error instead of being silently ignored. */
    bool CheckKnownFields(const FJsonObject& Object, std::initializer_list<const TCHAR*> KnownFields, const FString& Context, FString& OutError)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
        {
            bool bKnown = false;
            for (const TCHAR* Field : KnownFields)
            {
                if (Pair.Key.Equals(Field, ESearchCase::CaseSensitive))
                {
                    bKnown = true;
                    break;
                }
            }
            if (!bKnown)
            {
                OutError = FString::Printf(TEXT("%s: unknown field \"%s\""), *Context, *Pair.Key);
                return false;
            }
        }
        return true;
    }

    bool ReadString(const FJsonObject& Object, const TCHAR* Field, FString& InOutValue, const FString& Context, FString& OutError)
    {
        if (!Object.HasField(Field))
        {
            return true;
        }
        if (!Object.TryGetStringField(Field, InOutValue))
        {
            OutError = FString::Printf(TEXT("%s: \"%s\" must be a string"), *Context, Field);
            return false;
        }
        return true;
    }

    bool ReadBool(const FJsonObject& Object, const TCHAR* Field, bool& InOutValue, const FString& Context, FString& OutError)
    {
        if (!Object.HasField(Field))
        {
            return true;
        }
        if (!Object.TryGetBoolField(Field, InOutValue))
        {
            OutError = FString::Printf(TEXT("%s: \"%s\" must be true or false"), *Context, Field);
            return false;
        }
        return true;
    }

    bool ReadInt(const FJsonObject& Object, const TCHAR* Field, int32& InOutValue, const FString& Context, FString& OutError)
    {
        if (!Object.HasField(Field))
        {
            return true;
        }
        if (!Object.TryGetNumberField(Field, InOutValue))
        {
            OutError = FString::Printf(TEXT("%s: \"%s\" must be a number"), *Context, Field);
            return false;
        }
        return true;
    }

    bool ParseChannel(const FString& Text, EChannelPackerChannel& OutChannel)
    {
        const FString Name = Text.TrimStartAndEnd();
        if (Name.Equals(TEXT("R"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Red"), ESearchCase::IgnoreCase))
        {
            OutChannel = EChannelPackerChannel::Red;
        }
        else if (Name.Equals(TEXT("G"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Green"), ESearchCase::IgnoreCase))
        {
            OutChannel = EChannelPackerChannel::Green;
        }
        else if (Name.Equals(TEXT("B"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Blue"), ESearchCase::IgnoreCase))
        {
            OutChannel = EChannelPackerChannel::Blue;
        }
        else if (Name.Equals(TEXT("A"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Alpha"), ESearchCase::IgnoreCase))
        {
            OutChannel = EChannelPackerChannel::Alpha;
        }
        else
        {
            return false;
        }
        return true;
    }

    /** Reads the shared "overwrite" / "save" / "dryRun" fields. */
    bool ReadWriteOptions(const FJsonObject& Job, bool bForceDryRun, EChannelPackerOverwritePolicy& OutPolicy, bool& OutSave, bool& OutDryRun, const FString& Context, FString& OutError)
    {
        FString Overwrite;
        if (!ReadString(Job, TEXT("overwrite"), Overwrite, Context, OutError))
        {
            return false;
        }
        if (!Overwrite.IsEmpty())
        {
            if (Overwrite.Equals(TEXT("fail"), ESearchCase::IgnoreCase))
            {
                OutPolicy = EChannelPackerOverwritePolicy::Fail;
            }
            else if (Overwrite.Equals(TEXT("overwrite"), ESearchCase::IgnoreCase))
            {
                OutPolicy = EChannelPackerOverwritePolicy::Overwrite;
            }
            else if (Overwrite.Equals(TEXT("skip"), ESearchCase::IgnoreCase))
            {
                OutPolicy = EChannelPackerOverwritePolicy::Skip;
            }
            else
            {
                OutError = FString::Printf(TEXT("%s: \"overwrite\" must be \"fail\", \"overwrite\" or \"skip\" (got \"%s\")"), *Context, *Overwrite);
                return false;
            }
        }

        if (!ReadBool(Job, TEXT("save"), OutSave, Context, OutError) ||
            !ReadBool(Job, TEXT("dryRun"), OutDryRun, Context, OutError))
        {
            return false;
        }
        OutDryRun = OutDryRun || bForceDryRun;
        return true;
    }

    bool ApplyPresetField(const FJsonObject& Job, const FString& Context, FChannelPackerPreset& OutPreset, bool& bOutHasPreset, FString& OutError)
    {
        FString PresetName;
        if (!ReadString(Job, TEXT("preset"), PresetName, Context, OutError))
        {
            return false;
        }
        bOutHasPreset = !PresetName.IsEmpty();
        if (bOutHasPreset && !TextureChannelPackerCore::FindPreset(PresetName, OutPreset))
        {
            TArray<FString> Names;
            for (const FChannelPackerPreset& Preset : TextureChannelPackerCore::GetAllPresets())
            {
                Names.Add(Preset.PresetName);
            }
            OutError = FString::Printf(TEXT("%s: unknown preset \"%s\" (available: %s)"), *Context, *PresetName, *FString::Join(Names, TEXT(", ")));
            return false;
        }
        return true;
    }

    /** Parses one entry of a pack job's "inputs": either an asset path or { "texture", "channel", "invert" }. */
    bool ParsePackInput(const FJsonValue& Value, FChannelPackerInput& OutInput, const FString& Context, FString& OutError)
    {
        FString TexturePath;
        if (Value.Type == EJson::String)
        {
            TexturePath = Value.AsString();
        }
        else if (Value.Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value.AsObject();
            if (!CheckKnownFields(*Object, { TEXT("texture"), TEXT("channel"), TEXT("invert") }, Context, OutError))
            {
                return false;
            }
            if (!Object->TryGetStringField(TEXT("texture"), TexturePath))
            {
                OutError = FString::Printf(TEXT("%s: \"texture\" (asset path) is required"), *Context);
                return false;
            }

            FString ChannelText;
            if (!ReadString(*Object, TEXT("channel"), ChannelText, Context, OutError))
            {
                return false;
            }
            if (!ChannelText.IsEmpty() && !ParseChannel(ChannelText, OutInput.SourceChannel))
            {
                OutError = FString::Printf(TEXT("%s: \"channel\" must be R, G, B or A (got \"%s\")"), *Context, *ChannelText);
                return false;
            }

            if (!ReadBool(*Object, TEXT("invert"), OutInput.bInvert, Context, OutError))
            {
                return false;
            }
        }
        else
        {
            OutError = FString::Printf(TEXT("%s: expected an asset path or an object"), *Context);
            return false;
        }

        if (TexturePath.TrimStartAndEnd().IsEmpty())
        {
            OutError = FString::Printf(TEXT("%s: the texture path is empty"), *Context);
            return false;
        }
        OutInput.Texture = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(NormalizeAssetPath(TexturePath)));
        return true;
    }

    bool ParsePackJob(const FJsonObject& Job, bool bForceDryRun, FChannelPackerPackRequest& OutRequest, FString& OutError)
    {
        const FString Context = TEXT("pack job");
        if (!CheckKnownFields(Job, {
                TEXT("mode"), TEXT("preset"), TEXT("inputs"), TEXT("width"), TEXT("height"), TEXT("outputPath"),
                TEXT("outputName"), TEXT("suffix"), TEXT("compression"), TEXT("overwrite"), TEXT("save"), TEXT("dryRun") },
                Context, OutError))
        {
            return false;
        }

        // The preset provides defaults; explicit fields below override them.
        FChannelPackerPreset Preset;
        bool bHasPreset = false;
        if (!ApplyPresetField(Job, Context, Preset, bHasPreset, OutError))
        {
            return false;
        }
        if (bHasPreset)
        {
            TextureChannelPackerCore::ApplyPresetToPackRequest(Preset, OutRequest);
        }

        const TSharedPtr<FJsonObject>* Inputs = nullptr;
        if (!Job.TryGetObjectField(TEXT("inputs"), Inputs) || !Inputs || !Inputs->IsValid())
        {
            OutError = FString::Printf(TEXT("%s: \"inputs\" object (keys R/G/B/A) is required"), *Context);
            return false;
        }
        if (!CheckKnownFields(**Inputs, { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }, TEXT("inputs"), OutError))
        {
            return false;
        }

        FChannelPackerInput* Slots[4] = { &OutRequest.Red, &OutRequest.Green, &OutRequest.Blue, &OutRequest.Alpha };
        for (int32 Index = 0; Index < 4; ++Index)
        {
            const TSharedPtr<FJsonValue> Value = (*Inputs)->TryGetField(GSlotLetters[Index]);
            if (!Value.IsValid() || Value->IsNull())
            {
                continue;
            }
            if (!ParsePackInput(*Value, *Slots[Index], FString::Printf(TEXT("inputs.%s"), GSlotLetters[Index]), OutError))
            {
                return false;
            }
        }

        return ReadInt(Job, TEXT("width"), OutRequest.Width, Context, OutError)
            && ReadInt(Job, TEXT("height"), OutRequest.Height, Context, OutError)
            && ReadString(Job, TEXT("outputPath"), OutRequest.OutputPath, Context, OutError)
            && ReadString(Job, TEXT("outputName"), OutRequest.OutputName, Context, OutError)
            && ReadString(Job, TEXT("suffix"), OutRequest.FileNameSuffix, Context, OutError)
            && ReadString(Job, TEXT("compression"), OutRequest.Compression, Context, OutError)
            && ReadWriteOptions(Job, bForceDryRun, OutRequest.OverwritePolicy, OutRequest.bSave, OutRequest.bDryRun, Context, OutError);
    }

    bool ParseUnpackJob(const FJsonObject& Job, bool bForceDryRun, FChannelPackerUnpackRequest& OutRequest, FString& OutError)
    {
        const FString Context = TEXT("unpack job");
        if (!CheckKnownFields(Job, {
                TEXT("mode"), TEXT("preset"), TEXT("source"), TEXT("channels"), TEXT("outputPath"), TEXT("baseName"),
                TEXT("suffixes"), TEXT("overwrite"), TEXT("save"), TEXT("dryRun") },
                Context, OutError))
        {
            return false;
        }

        FChannelPackerPreset Preset;
        bool bHasPreset = false;
        if (!ApplyPresetField(Job, Context, Preset, bHasPreset, OutError))
        {
            return false;
        }
        if (bHasPreset)
        {
            TextureChannelPackerCore::ApplyPresetToUnpackRequest(Preset, OutRequest);
        }

        FString SourcePath;
        if (!Job.TryGetStringField(TEXT("source"), SourcePath) || SourcePath.TrimStartAndEnd().IsEmpty())
        {
            OutError = FString::Printf(TEXT("%s: \"source\" (asset path) is required"), *Context);
            return false;
        }
        OutRequest.Source = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(NormalizeAssetPath(SourcePath)));

        // "auto" (default): every channel except uniform ones. "all": every channel.
        // ["R", "G", ...]: exactly those channels, uniform or not.
        bool* ExportFlags[4] = { &OutRequest.bExportRed, &OutRequest.bExportGreen, &OutRequest.bExportBlue, &OutRequest.bExportAlpha };
        const TSharedPtr<FJsonValue> Channels = Job.TryGetField(TEXT("channels"));
        if (Channels.IsValid() && !Channels->IsNull())
        {
            if (Channels->Type == EJson::String)
            {
                const FString Mode = Channels->AsString();
                if (Mode.Equals(TEXT("all"), ESearchCase::IgnoreCase))
                {
                    OutRequest.bSkipUniformChannels = false;
                }
                else if (!Mode.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
                {
                    OutError = FString::Printf(TEXT("%s: \"channels\" must be \"auto\", \"all\" or an array such as [\"R\", \"G\"] (got \"%s\")"), *Context, *Mode);
                    return false;
                }
            }
            else if (Channels->Type == EJson::Array)
            {
                for (bool* Flag : ExportFlags)
                {
                    *Flag = false;
                }
                OutRequest.bSkipUniformChannels = false;

                for (const TSharedPtr<FJsonValue>& Entry : Channels->AsArray())
                {
                    EChannelPackerChannel Channel;
                    if (!Entry.IsValid() || Entry->Type != EJson::String || !ParseChannel(Entry->AsString(), Channel))
                    {
                        OutError = FString::Printf(TEXT("%s: \"channels\" entries must be R, G, B or A"), *Context);
                        return false;
                    }
                    *ExportFlags[(int32)Channel] = true;
                }
            }
            else
            {
                OutError = FString::Printf(TEXT("%s: \"channels\" must be \"auto\", \"all\" or an array such as [\"R\", \"G\"]"), *Context);
                return false;
            }
        }

        const TSharedPtr<FJsonObject>* Suffixes = nullptr;
        if (Job.HasField(TEXT("suffixes")))
        {
            if (!Job.TryGetObjectField(TEXT("suffixes"), Suffixes) || !Suffixes || !Suffixes->IsValid())
            {
                OutError = FString::Printf(TEXT("%s: \"suffixes\" must be an object with keys R/G/B/A"), *Context);
                return false;
            }
            if (!CheckKnownFields(**Suffixes, { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") }, TEXT("suffixes"), OutError))
            {
                return false;
            }
            FString* SuffixFields[4] = { &OutRequest.SuffixRed, &OutRequest.SuffixGreen, &OutRequest.SuffixBlue, &OutRequest.SuffixAlpha };
            for (int32 Index = 0; Index < 4; ++Index)
            {
                if (!ReadString(**Suffixes, GSlotLetters[Index], *SuffixFields[Index], TEXT("suffixes"), OutError))
                {
                    return false;
                }
            }
        }

        return ReadString(Job, TEXT("outputPath"), OutRequest.OutputPath, Context, OutError)
            && ReadString(Job, TEXT("baseName"), OutRequest.BaseName, Context, OutError)
            && ReadWriteOptions(Job, bForceDryRun, OutRequest.OverwritePolicy, OutRequest.bSave, OutRequest.bDryRun, Context, OutError);
    }

    FChannelPackerResult MakeInvalidJobResult(const FString& Error)
    {
        FChannelPackerResult Result;
        Result.Status = EChannelPackerStatus::Failed;
        Result.ErrorCode = TEXT("ErrorInvalidJob");
        Result.Message = Error;
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Strings)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const FString& String : Strings)
        {
            Values.Add(MakeShared<FJsonValueString>(String));
        }
        return Values;
    }

    TSharedRef<FJsonObject> ResultToJson(int32 Index, const FString& Mode, const FChannelPackerResult& Result)
    {
        TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetNumberField(TEXT("index"), Index);
        Json->SetStringField(TEXT("mode"), Mode);
        Json->SetStringField(TEXT("status"), TextureChannelPackerCore::StatusToString(Result.Status));
        Json->SetStringField(TEXT("errorCode"), Result.ErrorCode);
        Json->SetStringField(TEXT("message"), Result.Message);
        Json->SetArrayField(TEXT("outputs"), ToJsonStringArray(Result.OutputAssets));

        TArray<TSharedPtr<FJsonValue>> Warnings;
        for (const FChannelPackerWarning& Warning : Result.Warnings)
        {
            TSharedRef<FJsonObject> WarningJson = MakeShared<FJsonObject>();
            WarningJson->SetStringField(TEXT("code"), Warning.Code);
            WarningJson->SetStringField(TEXT("channel"), Warning.Channel);
            WarningJson->SetStringField(TEXT("message"), Warning.Message);
            Warnings.Add(MakeShared<FJsonValueObject>(WarningJson));
        }
        Json->SetArrayField(TEXT("warnings"), Warnings);

        Json->SetNumberField(TEXT("width"), Result.Width);
        Json->SetNumberField(TEXT("height"), Result.Height);
        if (Mode == TEXT("unpack"))
        {
            Json->SetArrayField(TEXT("skippedUniformChannels"), ToJsonStringArray(Result.SkippedUniformChannels));
        }
        return Json;
    }

    /** Writes the result document to ResultPath, or to the log when no path was given. */
    bool WriteResult(const TSharedRef<FJsonObject>& Root, const FString& ResultPath)
    {
        FString Output;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
        FJsonSerializer::Serialize(Root, Writer);

        if (ResultPath.IsEmpty())
        {
            UE_LOG(LogTexturePacker, Display, TEXT("%s"), *Output);
            return true;
        }
        if (!FFileHelper::SaveStringToFile(Output, *ResultPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogTexturePacker, Error, TEXT("Failed to write result file: %s"), *ResultPath);
            return false;
        }
        UE_LOG(LogTexturePacker, Display, TEXT("Wrote result file: %s"), *ResultPath);
        return true;
    }

    TSharedRef<FJsonObject> MakeResultRoot()
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(TEXT("version"), ResultSchemaVersion);
        return Root;
    }

    /** Writes a result document for a run that could not start (bad arguments or job file). */
    int32 FailRun(const FString& ResultPath, const TCHAR* Code, const FString& Message)
    {
        UE_LOG(LogTexturePacker, Error, TEXT("[%s] %s"), Code, *Message);

        TSharedRef<FJsonObject> Root = MakeResultRoot();
        TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
        Error->SetStringField(TEXT("code"), Code);
        Error->SetStringField(TEXT("message"), Message);
        Root->SetObjectField(TEXT("error"), Error);
        Root->SetArrayField(TEXT("results"), TArray<TSharedPtr<FJsonValue>>());
        WriteResult(Root, ResultPath);
        return ExitUsageError;
    }
}

UTextureChannelPackerCommandlet::UTextureChannelPackerCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;

    HelpDescription = TEXT("Runs Texture Channel Packer pack/unpack jobs from a JSON file and writes a JSON result file.");
    HelpUsage = TEXT("<Project>.uproject -run=TextureChannelPacker -Job=<jobs.json> [-Result=<result.json>] [-DryRun]");
}

int32 UTextureChannelPackerCommandlet::Main(const FString& Params)
{
    // bShouldStopOnSeparator = false: paths may contain ',' or ')' (quote paths that contain spaces).
    FString ResultPath;
    if (FParse::Value(*Params, TEXT("Result="), ResultPath, false) && !ResultPath.IsEmpty())
    {
        ResultPath = ResolveCommandLinePath(ResultPath);
    }

    FString JobPath;
    if (!FParse::Value(*Params, TEXT("Job="), JobPath, false) || JobPath.IsEmpty())
    {
        return FailRun(ResultPath, TEXT("ErrorUsage"), FString::Printf(TEXT("Usage: %s"), *HelpUsage));
    }
    JobPath = ResolveCommandLinePath(JobPath);

    const bool bForceDryRun = FParse::Param(*Params, TEXT("DryRun"));

    FString JobText;
    if (!FFileHelper::LoadFileToString(JobText, *JobPath))
    {
        return FailRun(ResultPath, TEXT("ErrorJobFileNotFound"), FString::Printf(TEXT("Cannot read job file: %s"), *JobPath));
    }

    TSharedPtr<FJsonObject> JobRoot;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JobText);
    if (!FJsonSerializer::Deserialize(Reader, JobRoot) || !JobRoot.IsValid())
    {
        return FailRun(ResultPath, TEXT("ErrorInvalidJobFile"), FString::Printf(TEXT("Job file is not a JSON object: %s (%s)"), *JobPath, *Reader->GetErrorMessage()));
    }

    const TArray<TSharedPtr<FJsonValue>>* Jobs = nullptr;
    if (!JobRoot->TryGetArrayField(TEXT("jobs"), Jobs) || !Jobs)
    {
        return FailRun(ResultPath, TEXT("ErrorInvalidJobFile"), TEXT("Job file must contain a \"jobs\" array."));
    }

    UE_LOG(LogTexturePacker, Display, TEXT("Running %d job(s) from %s%s"), Jobs->Num(), *JobPath, bForceDryRun ? TEXT(" (dry run)") : TEXT(""));

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 NumFailed = 0;

    for (int32 Index = 0; Index < Jobs->Num(); ++Index)
    {
        FString Mode;
        FChannelPackerResult Result;
        FString ParseError;

        const TSharedPtr<FJsonObject>* Job = nullptr;
        if (!(*Jobs)[Index].IsValid() || !(*Jobs)[Index]->TryGetObject(Job) || !Job || !Job->IsValid())
        {
            Result = MakeInvalidJobResult(TEXT("each job must be a JSON object"));
        }
        else
        {
            (*Job)->TryGetStringField(TEXT("mode"), Mode);
            if (Mode == TEXT("pack"))
            {
                FChannelPackerPackRequest Request;
                Result = ParsePackJob(**Job, bForceDryRun, Request, ParseError)
                    ? TextureChannelPackerCore::Pack(Request)
                    : MakeInvalidJobResult(ParseError);
            }
            else if (Mode == TEXT("unpack"))
            {
                FChannelPackerUnpackRequest Request;
                Result = ParseUnpackJob(**Job, bForceDryRun, Request, ParseError)
                    ? TextureChannelPackerCore::Unpack(Request)
                    : MakeInvalidJobResult(ParseError);
            }
            else
            {
                Result = MakeInvalidJobResult(FString::Printf(TEXT("\"mode\" must be \"pack\" or \"unpack\" (got \"%s\")"), *Mode));
            }
        }

        if (!Result.IsSuccess())
        {
            ++NumFailed;
        }
        const FString ModeLabel = Mode.IsEmpty() ? FString(TEXT("?")) : Mode;
        const FString ErrorLabel = Result.ErrorCode.IsEmpty() ? FString() : FString::Printf(TEXT(" [%s]"), *Result.ErrorCode);
        UE_LOG(LogTexturePacker, Display, TEXT("Job %d (%s): %s%s %s"),
            Index, *ModeLabel, *TextureChannelPackerCore::StatusToString(Result.Status), *ErrorLabel, *Result.Message);

        Results.Add(MakeShared<FJsonValueObject>(ResultToJson(Index, Mode, Result)));

        // Saved outputs are unpinned by the core and loaded inputs are not referenced any more, so
        // collect them between jobs to keep memory flat over long job files. Unsaved outputs stay
        // rooted and therefore remain available to later jobs.
        CollectGarbage(RF_NoFlags);
    }

    // Let texture builds kicked off by unsaved (or already saved) outputs finish before the editor exits.
    FAssetCompilingManager::Get().FinishAllCompilation();

    TSharedRef<FJsonObject> Root = MakeResultRoot();
    Root->SetNumberField(TEXT("total"), Jobs->Num());
    Root->SetNumberField(TEXT("failed"), NumFailed);
    Root->SetArrayField(TEXT("results"), Results);
    if (!WriteResult(Root, ResultPath))
    {
        return ExitUsageError;
    }

    UE_LOG(LogTexturePacker, Display, TEXT("Finished: %d job(s), %d failed"), Jobs->Num(), NumFailed);
    return NumFailed > 0 ? ExitJobsFailed : ExitSuccess;
}
