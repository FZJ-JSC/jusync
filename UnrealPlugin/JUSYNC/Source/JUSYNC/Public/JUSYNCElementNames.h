#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"

namespace JUSYNCElementNames
{
    // VTK's ANARI mapper names geometry "vtk_actor_<id>_<representation>_<n>".
    // ParaView recreates the actor across frames, so <id> is not stable even
    // when the visible element is the same. Fold that id away so one UE actor
    // and one cache key represent the element across time. The representation
    // suffix (e.g. "triangles_0", "spheres_0") stays intact, so genuinely
    // distinct geometry still gets distinct keys.
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
        // Stage postfixes the sender inserts between the prim name and the
        // "__<time>" marker of a clip key ("spheres_0_u1_Geom__0.000000"). Left in
        // place, the uniquifier is no longer the last segment, so a prim and its
        // rebuilt twin land in different groups and no duplicate is detected.
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

        // Peels whatever sits between the prim name and its "_u<k>" rebuild
        // uniquifier - a stage postfix, an embedded rank mark, or both - and
        // retries until the uniquifier is exposed. Peeling is lossless: the rank
        // marks are appended back to the element, so distinct elements stay
        // distinct and only the duplicate detection improves.
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

    // Morph, ramp and timeline state is keyed by (element, owning rank), so build
    // the string in one place. If a producer and a consumer disagree, two ranks of
    // one partitioned element share a morph chain and blend each other's previous
    // positions; an anonymous element falls back to a stable placeholder so it
    // still gets a key of its own.
    inline FString MakeRankedElementKey(const FString& Element, int32 Rank)
    {
        const FString Base = Element.IsEmpty() ? FString(TEXT("jusync_pc")) : Element;
        return Base + TEXT("_r") + FString::FromInt(FMath::Max(Rank, 0));
    }

    // Producer rank encoded in a clip name: "_r<digits>_" immediately after the
    // last "__" (e.g. "Wave_Geom__r3_7_s2.geo" -> 3). The rank suffix is what
    // keeps the pieces of one element spread over several ranks from sharing a
    // merge or a morph key. Returns false when the mark is absent.
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

    // Splits a rank mark off the end, then the rebuild uniquifier off what is
    // left. The rank mark is kept in the element so the pieces of one element
    // spread over several ranks never compete in the same visibility election -
    // every rank must stay visible for its timestep.
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
        }

        // Multi-rank clip keys carry the owner before the time ("__r1_0.500000");
        // every non-numeric token of that tail belongs to the element, so ranks
        // stay in separate groups.
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
