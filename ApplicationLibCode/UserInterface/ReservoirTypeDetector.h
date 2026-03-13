/**
 * @file ReservoirTypeDetector.h
 * @brief Full-dimension reservoir type judgment (Revised model hierarchy: Base + Enhanced + Sub-model)
 * @note Adapted to ResInsight official API (RigEclipseCaseData)
 */
#pragma once

// ResInsight Core Dependencies
#include "RiaDefines.h"
#include "RigActiveCellInfo.h"
#include "RigCaseCellResultsData.h"
#include "RigEclipseCaseData.h"
#include "RigEclipseResultAddress.h"
#include "RigEclipseResultInfo.h"
#include "RigMainGrid.h"
#include "RimEclipseCase.h"
#include "RimEclipseInputCase.h"

// Standard C++/Qt Headers
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cmath>
#include <map>
#include <set>

// -------------------------- 1. Enumeration Definitions --------------------------
enum class StructuralTrapType
{
    UNKNOWN_STRUCTURAL,
    ANTICLINE,
    FAULT_BLOCK,
    GAS_CAP,
    UNCONFORMITY,
    ONLAP,
    SAND_LENS,
    LITHOLOGIC_PINCHOUT
};

enum class LithologyType
{
    UNKNOWN_LITHOLOGY,
    UNCONSOLIDATED_SAND,
    NORMAL_SANDSTONE,
    TIGHT_SANDSTONE,
    COAL_BED,
    POROUS_LIMESTONE,
    FRACTURED_LIMESTONE,
    FRACTURED_CARBONATE,
    VUGGY_FRACTURED_LIMESTONE,
    BIOHERM_BEACH
};

enum class SpecialMediumType
{
    NONE,
    FRACTURED_SANDSTONE,
    FRACTURED_CARBONATE
};

enum class BaseSimulationModel
{
    UNKNOWN_BASE,
    BLACK_OIL,
    COMPONENT
};

enum class EnhancedSimulationModel
{
    NONE,
    THERMAL,
    CHEMICAL
};

enum class SubSimulationModel
{
    UNKNOWN_SUB,
    // Black Oil + No enhancement
    REGULAR_BLACK_OIL,
    LOW_VOLATILITY_OIL,
    REGULAR_WATERFLOOD,
    LOW_VOLATILITY_WATERFLOOD_BO,
    // Black Oil + Thermal
    STEAM_FLOOD_BO,
    STEAM_SOAK_BO,
    SAGD_BO,
    // Black Oil + Chemical
    POLYMER_FLOOD_BO,
    ASP_FLOOD_BO,
    SURFACTANT_FLOOD_BO,
    THERMAL_CHEMICAL_BO,
    // Component + No enhancement
    CONDENSATE_OIL_CM,
    VOLATILE_OIL_CM,
    GAS_CAP_OIL_CM,
    GAS_INJECTION_CM,
    // Component + Rare
    STEAM_FLOOD_CM,
    POLYMER_FLOOD_CM
};

// -------------------------- 2. Struct Definitions --------------------------
struct SingleTypeJudgeResult
{
    QString typeName;
    bool    isJudgable;
    bool    isMatched;
    QString judgeReason;
    float   confidence; // 0.0-1.0
};

struct ReservoirMultiDimResult
{
    // Geological Results
    QMap<StructuralTrapType, SingleTypeJudgeResult> structuralResults;
    QMap<LithologyType, SingleTypeJudgeResult>      lithologyResults;
    QMap<SpecialMediumType, SingleTypeJudgeResult>  specialResults;

    // Simulation Model Results
    SingleTypeJudgeResult                                baseModelResult;
    QSet<EnhancedSimulationModel>                        enhancedModels;
    QMap<EnhancedSimulationModel, SingleTypeJudgeResult> enhancedModelResults;
    QMap<SubSimulationModel, SingleTypeJudgeResult>      subModelResults;
    SingleTypeJudgeResult                                primarySubModelResult;

    // Method Declarations
    QString     getSimulationFullTag();
    QStringList getFullDimensionTags();
};

// -------------------------- 3. Core Class Declaration --------------------------
class ReservoirTypeJudger
{
public:
    // Constructor (Correct ResInsight API)
    ReservoirTypeJudger( RigEclipseCaseData* rigCaseData, RimEclipseCase* eclipseCase );

    // Main Judgment Entry
    ReservoirMultiDimResult judgeFullDimension();

private:
    // -------------------------- Core Auxiliary Functions --------------------------
    // Case-insensitive keyword existence check (Fix: Correct API)
    bool isKeywordExist( const QString& key );

    // Get all Eclipse keywords (Cache for performance)
    QSet<QString> getAllEclipseKeywords();

    // -------------------------- Other Auxiliary Functions --------------------------
    QString               getEclipseKeywordValue( const QString& key );
    QMap<QString, double> calculateGridTopDepthStats();
    QMap<QString, double> calculatePorPermAvg();

    // -------------------------- Judgment Functions --------------------------
    // Geological Judgment (Placeholders)
    SingleTypeJudgeResult judgeAnticline();
    SingleTypeJudgeResult judgeFaultBlock();
    SingleTypeJudgeResult judgeGasCapStructural();
    SingleTypeJudgeResult judgeUnconformity();
    SingleTypeJudgeResult judgeOnlap();
    SingleTypeJudgeResult judgeSandLens();
    SingleTypeJudgeResult judgeLithologicPinchout();
    SingleTypeJudgeResult judgeUnconsolidatedSand();
    SingleTypeJudgeResult judgeNormalSandstone();
    SingleTypeJudgeResult judgeTightSandstone();
    SingleTypeJudgeResult judgeCoalBed();
    SingleTypeJudgeResult judgePorousLimestone();
    SingleTypeJudgeResult judgeFracturedLimestone();
    SingleTypeJudgeResult judgeFracturedCarbonate();
    SingleTypeJudgeResult judgeVuggyFracturedLimestone();
    SingleTypeJudgeResult judgeBiohermBeach();
    SingleTypeJudgeResult judgeFracturedSandstone();
    SingleTypeJudgeResult judgeFracturedCarbonateMedium();

    // Simulation Model Judgment (Core Fix)
    SingleTypeJudgeResult judgeBaseModel( BaseSimulationModel modelType );
    SingleTypeJudgeResult judgeEnhancedModel( EnhancedSimulationModel modelType );
    SingleTypeJudgeResult judgeSubModel( SubSimulationModel modelType );
    SingleTypeJudgeResult selectPrimarySubModel( const QMap<SubSimulationModel, SingleTypeJudgeResult>& subResults );

    // -------------------------- Member Variables --------------------------
    RigEclipseCaseData*           m_rigCaseData       = nullptr;
    RimEclipseCase*               m_eclipseCase       = nullptr;
    BaseSimulationModel           m_detectedBaseModel = BaseSimulationModel::UNKNOWN_BASE;
    QSet<EnhancedSimulationModel> m_detectedEnhancedModels;
    QSet<QString>                 m_cachedKeywords; // Cache keywords for performance
};