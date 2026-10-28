#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

namespace JUSYNCElementNames
{
    inline FString CanonicalActorIdentity(const FString& ElementName)
    {
        static const FString Prefix = TEXT("vtk_actor_");
        if (!ElementName.StartsWith(Prefix, ESearchCase::CaseSensitive))
        {
            return ElementName;
        }

        int32 DigitsEnd = Prefix.Len();
        while (DigitsEnd < ElementName.Len() && FChar::IsDigit(ElementName[DigitsEnd]))
        {
            ++DigitsEnd;
        }

        if (DigitsEnd > Prefix.Len() && DigitsEnd < ElementName.Len() && ElementName[DigitsEnd] == TEXT('_'))
        {
            return Prefix + ElementName.Mid(DigitsEnd + 1);
        }
        return ElementName;
    }

    enum class EPayload : uint8
    {
        RawGeo,
        Usd,
        Other
    };

    struct FVariant
    {
        FString Element;
        int32 Uniq = -1;
        EPayload Payload = EPayload::Other;

        FString GroupKey() const
        {
            const TCHAR* Class = (Payload == EPayload::RawGeo) ? TEXT("geo")
                : (Payload == EPayload::Usd) ? TEXT("usd") : TEXT("other");
            return Element + TEXT("@") + Class;
        }
    };

    namespace Private
    {
        inline bool AllDigits(const FString& S)
        {
            if (S.IsEmpty())
            {
                return false;
            }
            for (const TCHAR C : S)
            {
                if (!FChar::IsDigit(C))
                {
                    return false;
                }
            }
            return true;
        }
    }

    inline bool SplitUniquifier(const FString& Name, FString& OutBase, int32& OutUniq)
    {
        OutBase = Name;
        OutUniq = -1;

        const int32 LastUnderscore = Name.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (LastUnderscore == INDEX_NONE || LastUnderscore + 2 > Name.Len() - 1)
        {
            return false;
        }
        if (Name[LastUnderscore + 1] != TEXT('u') && Name[LastUnderscore + 1] != TEXT('U'))
        {
            return false;
        }

        const FString Tail = Name.Mid(LastUnderscore + 2);
        if (!Private::AllDigits(Tail))
        {
            return false;
        }

        OutBase = Name.Left(LastUnderscore);
        OutUniq = FCString::Atoi(*Tail);
        return true;
    }

    namespace Private
    {
        inline void StripStagePostfix(FString& Name)
        {
            static const TCHAR* const Postfixes[] = {
                TEXT("_Geom"), TEXT("_Field"), TEXT("_Material"),
                TEXT("_Sampler"), TEXT("_Camera"), TEXT("_Light")
            };
            for (const TCHAR* Postfix : Postfixes)
            {
                if (Name.EndsWith(Postfix, ESearchCase::CaseSensitive))
                {
                    Name = Name.Left(Name.Len() - static_cast<int32>(FCString::Strlen(Postfix)));
                    return;
                }
            }
        }

        inline void UnwindHead(FString Head, const FString& RankMark, FString& OutElement, int32& OutUniq)
        {
            FString Merged = RankMark;
            FString Base = Head;
            int32 Uniq = -1;

            for (int32 Pass = 0; Pass < 4; ++Pass)
            {
                if (SplitUniquifier(Head, Base, Uniq))
                {
                    break;
                }

                const FString Before = Head;
                StripStagePostfix(Head);

                const int32 RankUnderscore = Head.Find(TEXT("_r"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                if (RankUnderscore != INDEX_NONE && AllDigits(Head.Mid(RankUnderscore + 2)))
                {
                    Merged = Head.Mid(RankUnderscore) + Merged;
                    Head = Head.Left(RankUnderscore);
                }

                if (Head == Before)
                {
                    Base = Head;
                    Uniq = -1;
                    break;
                }
            }

            OutElement = Base + Merged;
            OutUniq = Uniq;
        }
    }

    inline FString MakeRankedElementKey(const FString& Element, int32 Rank)
    {
        const FString Base = Element.IsEmpty() ? FString(TEXT("jusync_pc")) : Element;
        return Base + TEXT("_r") + FString::FromInt(FMath::Max(Rank, 0));
    }

    inline bool ParseFileRank(const FString& Filename, int32& OutRank)
    {
        OutRank = INDEX_NONE;

        FString Name = FPaths::GetCleanFilename(Filename);
        Name = FPaths::GetBaseFilename(Name);

        const int32 Tail = Name.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (Tail == INDEX_NONE)
        {
            return false;
        }

        const FString After = Name.Mid(Tail + 2);
        if (!After.StartsWith(TEXT("r"), ESearchCase::CaseSensitive))
        {
            return false;
        }

        int32 DigitsEnd = 1;
        while (DigitsEnd < After.Len() && FChar::IsDigit(After[DigitsEnd]))
        {
            ++DigitsEnd;
        }
        if (DigitsEnd == 1 || DigitsEnd >= After.Len() || After[DigitsEnd] != TEXT('_'))
        {
            return false;
        }

        OutRank = FCString::Atoi(*After.Mid(1, DigitsEnd - 1));
        return OutRank >= 0;
    }

    inline FVariant ParseElement(const FString& ElementKey)
    {
        FVariant Out;

        FString Name = ElementKey.TrimStartAndEnd();
        FString RankMark;

        const int32 RankUnderscore = Name.Find(TEXT("_r"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (RankUnderscore != INDEX_NONE && Private::AllDigits(Name.Mid(RankUnderscore + 2)))
        {
            RankMark = Name.Mid(RankUnderscore);
            Name = Name.Left(RankUnderscore);
        }

        Private::UnwindHead(Name, RankMark, Out.Element, Out.Uniq);
        Out.Payload = EPayload::Other;
        return Out;
    }

    inline FVariant Parse(const FString& NameOrFilename)
    {
        FVariant Out;

        FString Name = FPaths::GetCleanFilename(NameOrFilename);
        const FString Ext = FPaths::GetExtension(Name).ToLower();
        Name = FPaths::GetBaseFilename(Name);

        if (Ext == TEXT("geo"))
        {
            Out.Payload = EPayload::RawGeo;
        }
        else if (Ext == TEXT("usd") || Ext == TEXT("usda") || Ext == TEXT("usdc") || Ext == TEXT("usdz"))
        {
            Out.Payload = EPayload::Usd;
        }
        else
        {
            Out.Payload = EPayload::Other;
        }

        FString Head = Name;
        FString Tail;
        const int32 TimeSep = Name.Find(TEXT("__"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (TimeSep != INDEX_NONE)
        {
            Head = Name.Left(TimeSep);
            Tail = Name.Mid(TimeSep + 2);

            const int32 SubsetSep = Tail.Find(TEXT("_s"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (SubsetSep != INDEX_NONE &&
                Private::AllDigits(Tail.Mid(SubsetSep + 2)))
            {
                Tail = Tail.Left(SubsetSep);
            }
        }

        FString RankMark;
        if (!Tail.IsEmpty())
        {
            TArray<FString> Tokens;
            Tail.ParseIntoArray(Tokens, TEXT("_"));
            for (const FString& Token : Tokens)
            {
                if (!Token.IsNumeric())
                {
                    RankMark += TEXT("_") + Token;
                }
            }
        }

        Private::UnwindHead(Head, RankMark, Out.Element, Out.Uniq);
        return Out;
    }
}
