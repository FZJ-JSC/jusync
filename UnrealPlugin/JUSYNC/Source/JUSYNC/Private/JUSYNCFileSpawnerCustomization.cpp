#include "JUSYNCFileSpawnerCustomization.h"

#if WITH_EDITOR

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Input/Reply.h"
#include "JUSYNCFileSpawnerActor.h"
#include "SSimpleTimeSlider.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
/**
 * Dropdown picker for cached samples. Selecting an entry scrubs to that
 * sample; "LIVE (newest)" resumes the live stream. The list is rebuilt only
 * when the cache length changes, and the current selection follows the
 * transport buttons/slider without feeding a change back into playback.
 */
class SJUSYNCSampleDropdown final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SJUSYNCSampleDropdown) {}
        SLATE_ARGUMENT(TWeakObjectPtr<AJUSYNCFileSpawnerActor>, Spawner)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs)
    {
        Spawner = InArgs._Spawner;

        ChildSlot
        [
            SNew(SBox)
            .WidthOverride(320.0f)
            [
                SAssignNew(ComboBox, SComboBox<TSharedPtr<int32>>)
                .OptionsSource(&Options)
                .OnGenerateWidget_Lambda([this](TSharedPtr<int32> Item)
                {
                    return SNew(STextBlock).Text(GetOptionText(Item));
                })
                .OnSelectionChanged_Lambda([this](TSharedPtr<int32> Item, ESelectInfo::Type SelectInfo)
                {
                    HandleSelectionChanged(Item, SelectInfo);
                })
                .InitiallySelectedItem(nullptr)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        return GetCurrentText();
                    })
                ]
            ]
        ];

        SetCanTick(true);
        RefreshOptions();
    }

    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
    {
        const int32 Count = GetCount();
        const bool bLive = IsLive();
        const int32 Index = GetCurrentIndex();
        if (Count != LastCount || bLive != bLastLive || Index != LastIndex)
        {
            RefreshOptions();
        }
    }

private:
    int32 GetCount() const
    {
        return Spawner.IsValid() ? Spawner->GetCachedSampleCount() : 0;
    }

    bool IsLive() const
    {
        return !Spawner.IsValid() || !Spawner->IsPlayingCachedSample();
    }

    int32 GetCurrentIndex() const
    {
        return Spawner.IsValid() ? Spawner->GetPlaybackSampleIndex() : 0;
    }

    FText GetOptionText(TSharedPtr<int32> Item) const
    {
        if (!Item.IsValid() || *Item == INDEX_NONE)
        {
            return FText::FromString(TEXT("LIVE (newest)"));
        }

        const int32 Count = LastCount > 0 ? LastCount : GetCount();
        const double Time = Spawner.IsValid() ? Spawner->GetCachedSampleTime(*Item) : -1.0;
        if (Time >= 0.0)
        {
            return FText::FromString(FString::Printf(
                TEXT("t=%.3g   [%d / %d]%s"),
                Time, *Item + 1, Count, (*Item == 0) ? TEXT("  (oldest)") : TEXT("")));
        }
        if (*Item == 0)
        {
            return FText::FromString(FString::Printf(TEXT("Sample 1 / %d (oldest)"), Count));
        }
        return FText::FromString(FString::Printf(TEXT("Sample %d / %d"), *Item + 1, Count));
    }

    FText GetCurrentText() const
    {
        if (IsLive())
        {
            return FText::FromString(TEXT("LIVE (newest)"));
        }

        const int32 Count = GetCount();
        if (Count <= 0)
        {
            return FText::FromString(TEXT("No samples"));
        }

        const int32 Index = FMath::Clamp(GetCurrentIndex(), 0, Count - 1);
        const double Time = Spawner.IsValid() ? Spawner->GetCachedSampleTime(Index) : -1.0;
        if (Time >= 0.0)
        {
            return FText::FromString(FString::Printf(
                TEXT("t=%.3g   [%d / %d]"), Time, Index + 1, Count));
        }
        return FText::FromString(FString::Printf(TEXT("Sample %d / %d"), Index + 1, Count));
    }

    void RefreshOptions()
    {
        if (!ComboBox.IsValid())
        {
            return;
        }

        LastCount = GetCount();
        bLastLive = IsLive();
        LastIndex = GetCurrentIndex();

        if (Options.Num() != LastCount + 1)
        {
            Options.Empty(LastCount + 1);
            Options.Add(MakeShared<int32>(INDEX_NONE));
            for (int32 Index = 0; Index < LastCount; ++Index)
            {
                Options.Add(MakeShared<int32>(Index));
            }

            bUpdatingSelection = true;
            ComboBox->RefreshOptions();
            bUpdatingSelection = false;
        }

        const int32 Desired = bLastLive
            ? INDEX_NONE
            : FMath::Clamp(LastIndex, 0, FMath::Max(0, LastCount - 1));

        TSharedPtr<int32> Selected;
        for (const TSharedPtr<int32>& Option : Options)
        {
            if (Option.IsValid() && *Option == Desired)
            {
                Selected = Option;
                break;
            }
        }

        if (Selected.IsValid() && ComboBox->GetSelectedItem() != Selected)
        {
            bUpdatingSelection = true;
            ComboBox->SetSelectedItem(Selected);
            bUpdatingSelection = false;
        }
    }

    void HandleSelectionChanged(TSharedPtr<int32> Item, ESelectInfo::Type SelectInfo)
    {
        if (bUpdatingSelection || !Item.IsValid() || !Spawner.IsValid())
        {
            return;
        }

        const int32 Index = *Item;
        if (Index == INDEX_NONE)
        {
            Spawner->FollowLatestSample();
            return;
        }

        const int32 Count = Spawner->GetCachedSampleCount();
        if (Count <= 0)
        {
            return;
        }

        Spawner->SetPlaybackPosition(Count > 1
            ? static_cast<float>(Index) / static_cast<float>(Count - 1)
            : 0.0f);
    }

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> Spawner;
    TSharedPtr<SComboBox<TSharedPtr<int32>>> ComboBox;
    TArray<TSharedPtr<int32>> Options;
    int32 LastCount = INDEX_NONE;
    int32 LastIndex = INDEX_NONE;
    bool bLastLive = true;
    bool bUpdatingSelection = false;
};
} // namespace

TSharedRef<IDetailCustomization> FJUSYNCFileSpawnerCustomization::MakeInstance()
{
    return MakeShareable(new FJUSYNCFileSpawnerCustomization());
}

void FJUSYNCFileSpawnerCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
    TArray<TWeakObjectPtr<UObject>> CustomizedObjects;
    DetailBuilder.GetObjectsBeingCustomized(CustomizedObjects);

    TWeakObjectPtr<AJUSYNCFileSpawnerActor> Spawner;
    for (const TWeakObjectPtr<UObject>& Object : CustomizedObjects)
    {
        if (AJUSYNCFileSpawnerActor* Actor = Cast<AJUSYNCFileSpawnerActor>(Object.Get()))
        {
            Spawner = Actor;
            break;
        }
    }

    if (!Spawner.IsValid())
    {
        return;
    }

    // The real slider replaces the raw normalized float row. The cache settings
    // and read-only counters remain as normal Details rows.
    DetailBuilder.HideProperty(
        GET_MEMBER_NAME_CHECKED(AJUSYNCFileSpawnerActor, PlaybackPosition));

    IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(
        TEXT("JUSYNC|Spawner|Playback"),
        FText::FromString(TEXT("JUSYNC Playback")),
        ECategoryPriority::Important);
    Category.InitiallyCollapsed(false);

    const auto PositionText = [Spawner]() -> FText
    {
        if (!Spawner.IsValid())
        {
            return FText::FromString(TEXT("--"));
        }

        const int32 Count = Spawner->GetCachedSampleCount();
        const int32 Index = Spawner->GetPlaybackSampleIndex();
        if (Count <= 0)
        {
            return FText::FromString(TEXT("no samples yet"));
        }

        const int32 Clamped = FMath::Clamp(Index, 0, Count - 1);
        const double Time = Spawner->GetCachedSampleTime(Clamped);
        if (Time >= 0.0)
        {
            return FText::FromString(FString::Printf(
                TEXT("t=%.3g   [%d / %d]"), Time, Clamped + 1, Count));
        }
        return FText::FromString(FString::Printf(TEXT("%d / %d"), Clamped + 1, Count));
    };

    Category.AddCustomRow(FText::FromString(TEXT("Cached Sample Playback")))
        .WholeRowContent()
        [
            SNew(SVerticalBox)

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 2.0f, 0.0f, 4.0f)
            [
                SNew(SHorizontalBox)

                + SHorizontalBox::Slot()
                .FillWidth(1.0f)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                        .Text(FText::FromString(TEXT("Sample Timeline")))
                        .ToolTipText(FText::FromString(
                            TEXT("Scrub the recent mesh/point-cloud samples cached in memory, "
                                 "or pick a sender time from the dropdown below. Moving left of "
                                 "the live edge pauses incoming geometry updates; choose LIVE to resume.")))
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                        .Text_Lambda(PositionText)
                ]
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            [
                // Same timeline scrubber widget that the DaySequence
                // time-of-day preview uses, but indexed by cached sample.
                SNew(SSimpleTimeSlider)
                    .IsEnabled_Lambda([Spawner]()
                    {
                        return Spawner.IsValid() && Spawner->GetCachedSampleCount() > 1;
                    })
                    .AllowPan(false)
                    .AllowZoom(false)
                    .DesiredSize(FVector2f(100.0f, 28.0f))
                    .ViewRange_Lambda([Spawner]()
                    {
                        const int32 Count = Spawner.IsValid() ? Spawner->GetCachedSampleCount() : 0;
                        return TRange<double>(0.0, Count > 1 ? static_cast<double>(Count - 1) : 1.0);
                    })
                    .ClampRange_Lambda([Spawner]()
                    {
                        const int32 Count = Spawner.IsValid() ? Spawner->GetCachedSampleCount() : 0;
                        return TRange<double>(0.0, Count > 1 ? static_cast<double>(Count - 1) : 1.0);
                    })
                    .ClampRangeHighlightSize(0.15f)
                    .ClampRangeHighlightColor(FLinearColor(0.15f, 0.45f, 0.9f, 0.35f))
                    .ScrubPosition_Lambda([Spawner]()
                    {
                        return Spawner.IsValid()
                            ? static_cast<double>(Spawner->GetPlaybackSampleIndex())
                            : 0.0;
                    })
                    .OnScrubPositionChanged_Lambda([Spawner](double NewScrubTime, bool /*bIsScrubbing*/)
                    {
                        if (!Spawner.IsValid())
                        {
                            return;
                        }

                        const int32 Count = Spawner->GetCachedSampleCount();
                        if (Count <= 1)
                        {
                            Spawner->SetPlaybackPosition(0.0f);
                            return;
                        }

                        const int32 Index = FMath::Clamp(
                            FMath::RoundToInt(NewScrubTime), 0, Count - 1);
                        Spawner->SetPlaybackPosition(
                            static_cast<float>(Index) / static_cast<float>(Count - 1));
                    })
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 4.0f, 0.0f, 0.0f)
            [
                SNew(SJUSYNCSampleDropdown)
                .Spawner(Spawner)
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.0f, 4.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)

                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                [
                    SNew(SButton)
                        .Text(FText::FromString(TEXT("Oldest")))
                        .ToolTipText(FText::FromString(TEXT("Jump to the oldest cached sample")))
                        .OnClicked_Lambda([Spawner]()
                        {
                            if (Spawner.IsValid())
                            {
                                Spawner->SetPlaybackPosition(0.0f);
                            }
                            return FReply::Handled();
                        })
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                [
                    SNew(SButton)
                        .Text(FText::FromString(TEXT("Previous")))
                        .ToolTipText(FText::FromString(TEXT("Step one cached sample backwards")))
                        .OnClicked_Lambda([Spawner]()
                        {
                            if (Spawner.IsValid())
                            {
                                Spawner->PlaybackPreviousSample();
                            }
                            return FReply::Handled();
                        })
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                [
                    SNew(SButton)
                        .Text(FText::FromString(TEXT("Next")))
                        .ToolTipText(FText::FromString(TEXT("Step one cached sample forwards")))
                        .OnClicked_Lambda([Spawner]()
                        {
                            if (Spawner.IsValid())
                            {
                                Spawner->PlaybackNextSample();
                            }
                            return FReply::Handled();
                        })
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                [
                    SNew(SButton)
                        .Text(FText::FromString(TEXT("Clear Cache")))
                        .ToolTipText(FText::FromString(TEXT("Drop all cached samples (live actors stay)")))
                        .OnClicked_Lambda([Spawner]()
                        {
                            if (Spawner.IsValid())
                            {
                                Spawner->ClearCachedSamples();
                            }
                            return FReply::Handled();
                        })
                ]
            ]
        ];
}

#endif
