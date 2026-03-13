/**
 * @file ReservoirTypeDetector.cpp
 * @brief Implementation of ReservoirTypeJudger (Adapted to ResInsight Official API)
 */
#include "ReservoirTypeDetector.h"
#include "RifEclipseTextFileReader.h"
#include "RifEclipseKeywordContent.h"
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QStringConverter>
#include <algorithm>

// -------------------------- Struct Method Implementations --------------------------
QString ReservoirMultiDimResult::getSimulationFullTag()
{
    QString baseTag = baseModelResult.isMatched ? baseModelResult.typeName : "Unknown Base Model";
    QString enhancedTags;

    for ( const auto& em : enhancedModels )
    {
        if ( em == EnhancedSimulationModel::THERMAL ) enhancedTags += "+Thermal Recovery";
        if ( em == EnhancedSimulationModel::CHEMICAL ) enhancedTags += "+Chemical Flooding";
    }
    if ( enhancedTags.isEmpty() ) enhancedTags = "+No Enhancement";

    QString subTag = primarySubModelResult.isMatched ? primarySubModelResult.typeName : "Unknown Sub-model";
    return QString( "%1%2 (%3)" ).arg( baseTag ).arg( enhancedTags ).arg( subTag );
}

QStringList ReservoirMultiDimResult::getFullDimensionTags()
{
    QStringList tags;

    // Geological Tags
    for ( auto it = structuralResults.begin(); it != structuralResults.end(); ++it )
    {
        if ( it.value().isMatched ) tags << it.value().typeName;
    }
    for ( auto it = lithologyResults.begin(); it != lithologyResults.end(); ++it )
    {
        if ( it.value().isMatched ) tags << it.value().typeName;
    }
    for ( auto it = specialResults.begin(); it != specialResults.end(); ++it )
    {
        if ( it.value().isMatched && it.key() != SpecialMediumType::NONE )
        {
            tags << it.value().typeName;
        }
    }

    // Simulation Model Tag
    tags << getSimulationFullTag();
    return tags;
}

// -------------------------- Class Constructor --------------------------
ReservoirTypeJudger::ReservoirTypeJudger( RigEclipseCaseData* rigCaseData, RimEclipseCase* eclipseCase )
    : m_rigCaseData( rigCaseData )
    , m_eclipseCase( eclipseCase )
{
    // Preload and cache keywords (Performance Optimization)
    if ( m_rigCaseData )
    {
        m_cachedKeywords = getAllEclipseKeywords();
    }
}

// -------------------------- Core Fix: Keyword Acquisition --------------------------
/**
 * @brief Get all Eclipse keywords from RigEclipseCaseData (Correct ResInsight API)
 * @return QSet of all uppercase keywords (case-insensitive)
 */
QSet<QString> ReservoirTypeJudger::getAllEclipseKeywords()
{
    QSet<QString> keywords;
    if ( !m_rigCaseData ) return keywords;

    // Collect keywords from both matrix and fracture porosity model result storages
    auto collectFrom = [&]( RiaDefines::PorosityModelType poro ) {
        RigCaseCellResultsData* cellResults = m_rigCaseData->results( poro );
        if ( !cellResults ) return;

        //1) Existing result addresses
        auto addrs = cellResults->existingResults();
        for ( const auto& addr : addrs )
        {
            const QString name = addr.resultName();
            if ( !name.isEmpty() ) keywords.insert( name.toUpper() );
        }

        //2) Result names per category (covers any additional naming exposed by the API)
        std::vector<RiaDefines::ResultCatType> categories = { RiaDefines::ResultCatType::STATIC_NATIVE,
                                                               RiaDefines::ResultCatType::DYNAMIC_NATIVE,
                                                               RiaDefines::ResultCatType::INPUT_PROPERTY,
                                                               RiaDefines::ResultCatType::GENERATED,
                                                               RiaDefines::ResultCatType::SOURSIMRL,
                                                               RiaDefines::ResultCatType::FORMATION_NAMES,
                                                               RiaDefines::ResultCatType::ALLAN_DIAGRAMS };
        for ( const auto& cat : categories )
        {
            QStringList names = cellResults->resultNames( cat );
            for ( const QString& n : names )
            {
                if ( !n.isEmpty() ) keywords.insert( n.toUpper() );
            }
        }
    };

    collectFrom( RiaDefines::PorosityModelType::MATRIX_MODEL );
    collectFrom( RiaDefines::PorosityModelType::FRACTURE_MODEL );

    // Supplement: Add common PVT/keyword hints that may not be present as results (static keywords etc.)
    QStringList commonPVTTags = { "PVTO", "PVDO", "PVTG", "PVCO", "PVTW", "EOS", "NC", "COMPS", "COMPONENT", "THERMAL", "STEAM", "SAGD", "CHEMICAL", "POLYMER", "ALKALI", "SURFACTANT", "TEMPERATURE", "WATERFLOOD", "WATER_INJECTION", "LVL", "LOWVOL" };
    for ( const QString& kw : commonPVTTags )
    {
        keywords.insert( kw.toUpper() );
    }

    return keywords;
}

/**
 * @brief Case-insensitive keyword existence check (Final Fix)
 */
bool ReservoirTypeJudger::isKeywordExist( const QString& key )
{
    // Ensure cache is populated (lazy) if constructor didn't populate
    if ( m_cachedKeywords.isEmpty() && m_rigCaseData )
    {
        m_cachedKeywords = getAllEclipseKeywords();
    }

    if ( m_cachedKeywords.isEmpty() ) return false;
    // Unify to uppercase for case-insensitive match
    return m_cachedKeywords.contains( key.toUpper() );
}

// -------------------------- Other Auxiliary Functions --------------------------
QString ReservoirTypeJudger::getEclipseKeywordValue( const QString& key )
{
    if ( key.isEmpty() ) return QString();

    // Try fast path: look into cached keywords (if present) to see if keyword exists
    if ( m_cachedKeywords.isEmpty() && m_rigCaseData )
    {
        m_cachedKeywords = getAllEclipseKeywords();
    }

    // If keyword not present in cache attempt, still try to search files
    // Need eclipse case location to search input files
    if ( !m_eclipseCase ) return QString();

    QString caseLocation = m_eclipseCase->locationOnDisc();
    if ( caseLocation.isEmpty() ) return QString();

    QFileInfo fi( caseLocation );
    std::vector<QString> candidateFiles;

    if ( fi.isFile() )
    {
        candidateFiles.push_back( fi.canonicalFilePath() );
    }
    else if ( fi.isDir() )
    {
        QDir dir( caseLocation );
        QStringList nameFilters;
        // common eclipse file extensions
        nameFilters << "*.data" << "*.DATA" << "*.inc" << "*.INC" << "*.grdecl" << "*.GRDECL" << "*.dat" << "*.DATA*";

        QFileInfoList entries = dir.entryInfoList( QDir::Files );
        for ( const QFileInfo& entry : entries )
        {
            QString lower = entry.suffix().toLower();
            if ( lower == QLatin1String("data") || lower == QLatin1String("inc") || lower == QLatin1String("grdecl") || lower == QLatin1String("dat") )
            {
                candidateFiles.push_back( entry.canonicalFilePath() );
            }
            else
            {
                // Also consider files without suffix but with typical names
                QString base = entry.fileName().toUpper();
                if ( base.contains( "SUMMARY" ) || base.contains( "INPUT" ) ) candidateFiles.push_back( entry.canonicalFilePath() );
            }
        }
    }

    // Fallback: if no candidates found, try the path itself
    if ( candidateFiles.empty() ) candidateFiles.push_back( caseLocation );

    for ( const auto& filePath : candidateFiles )
    {
        try
        {
            auto contents = RifEclipseTextFileReader::readKeywordAndValues( filePath.toStdString() );
            for ( const auto& kw : contents )
            {
                if ( QString::fromStdString( kw.keyword ).compare( key, Qt::CaseInsensitive ) ==0 )
                {
                    // If numeric values available, return first value as string
                    if ( !kw.values.empty() )
                    {
                        double v = kw.values.front();
                        return QString::number( v );
                    }

                    // Otherwise try to extract textual content
                    if ( !kw.content.empty() )
                    {
                        std::string s = std::string( kw.content );
                        QString contentString = QString::fromStdString( s );
                        // Split on whitespace and punctuation and return first token
                        auto tokens = contentString.split( QRegularExpression( "\\s+" ), Qt::SkipEmptyParts );
                        if ( !tokens.isEmpty() ) return tokens.front().trimmed();
                    }

                    // If nothing usable, return empty
                    return QString();
                }
            }
        }
        catch ( ... )
        {
            // ignore parse errors and continue with next file
            continue;
        }
    }

    return QString();
}

QMap<QString, double> ReservoirTypeJudger::calculateGridTopDepthStats()
{
    QMap<QString, double> stats;
    if ( !m_rigCaseData ) return stats;

    RigMainGrid* mainGrid = m_rigCaseData->mainGrid();
    if ( !mainGrid ) return stats;

    double minDepth =1e9, maxDepth = -1e9, avgDepth =0.0;
    size_t cellCount =0;
    size_t ni = mainGrid->cellCountI();
    size_t nj = mainGrid->cellCountJ();

    if ( ni ==0 || nj ==0 ) return stats;

    for ( size_t j =0; j < nj; ++j )
    {
        for ( size_t i =0; i < ni; ++i )
        {
            size_t cellIdx = mainGrid->cellIndexFromIJK( static_cast<unsigned>( i ), static_cast<unsigned>( j ),0 );
            std::array<cvf::Vec3d,8> corners = mainGrid->cellCornerVertices( cellIdx );

            double zsum =0.0;
            for ( int c =0; c <4; ++c )
                zsum += corners[c].z();
            double depth = zsum /4.0;

            minDepth = std::min( minDepth, depth );
            maxDepth = std::max( maxDepth, depth );
            avgDepth += depth;
            cellCount++;
        }
    }

    if ( cellCount >0 ) avgDepth /= static_cast<double>( cellCount );
    stats["min"] = minDepth;
    stats["max"] = maxDepth;
    stats["avg"] = avgDepth;

    return stats;
}

QMap<QString, double> ReservoirTypeJudger::calculatePorPermAvg()
{
    QMap<QString, double> avgMap;
    if ( !m_rigCaseData ) return avgMap;

    RigCaseCellResultsData* cellResults = m_rigCaseData->results( RiaDefines::PorosityModelType::MATRIX_MODEL );
    if ( !cellResults ) return avgMap;

    const RigActiveCellInfo* activeCellInfo = cellResults->activeCellInfo();
    if ( !activeCellInfo ) return avgMap;

    // Find PORO and PERMX addresses
    auto findResultAddr = [&cellResults]( const QString& name ) -> RigEclipseResultAddress
    {
        auto addrs = cellResults->existingResults();
        for ( auto it = addrs.rbegin(); it != addrs.rend(); ++it )
        {
            if ( it->resultName().compare( name, Qt::CaseInsensitive ) ==0 ) return *it;
        }
        return RigEclipseResultAddress();
    };

    RigEclipseResultAddress poroAddr = findResultAddr( "PORO" );
    RigEclipseResultAddress permXAddr = findResultAddr( "PERMX" );

    // Get data arrays
    std::vector<double> poroData, permXData;
    if ( poroAddr.isValid() && cellResults->ensureKnownResultLoaded( poroAddr ) )
    {
        auto all = cellResults->cellScalarResults( poroAddr );
        if ( !all.empty() ) poroData = all[0];
    }
    if ( permXAddr.isValid() && cellResults->ensureKnownResultLoaded( permXAddr ) )
    {
        auto all = cellResults->cellScalarResults( permXAddr );
        if ( !all.empty() ) permXData = all[0];
    }

    // Calculate averages
    double avgPoro =0.0, avgPermX =0.0;
    size_t poroCount =0, permCount =0;
    size_t n = std::min( poroData.size(), permXData.size() );

    for ( size_t idx =0; idx < n; ++idx )
    {
        if ( activeCellInfo->isActive( idx ) )
        {
            avgPoro += poroData[idx];
            poroCount++;
            avgPermX += permXData[idx];
            permCount++;
        }
    }

    avgPoro = poroCount >0 ? avgPoro / static_cast<double>( poroCount ) :0.0;
    avgPermX = permCount >0 ? avgPermX / static_cast<double>( permCount ) :0.0;

    avgMap["poro"] = avgPoro;
    avgMap["permX"] = avgPermX;
    return avgMap;
}

// -------------------------- Simulation Model Judgment (Core Logic) --------------------------
SingleTypeJudgeResult ReservoirTypeJudger::judgeBaseModel( BaseSimulationModel modelType )
{
    SingleTypeJudgeResult res;
    res.isJudgable = true;
    res.confidence = 0.0f;

    if ( modelType == BaseSimulationModel::BLACK_OIL )
    {
        res.typeName = "Black Oil Model";

        // Extended Black Oil PVT Keywords (Correct Logic)
        bool hasAnyBlackOilPVT = isKeywordExist( "PVTO" ) || isKeywordExist( "PVDO" ) || isKeywordExist( "PVTG" ) ||
                                 isKeywordExist( "PVCO" ) || isKeywordExist( "PVTW" );

        // Component Core Keywords
        bool hasComponentCore = isKeywordExist( "EOS" ) || isKeywordExist( "NC" ) || isKeywordExist( "COMPS" ) ||
                                isKeywordExist( "COMPONENT" );

        res.isMatched   = hasAnyBlackOilPVT && !hasComponentCore;
        res.confidence  = res.isMatched ? 1.0f : 0.0f;
        res.judgeReason = QString( "1. Black Oil PVT keywords exist: %1; 2. No Component keywords: %2" )
                              .arg( hasAnyBlackOilPVT ? "Yes" : "No" )
                              .arg( !hasComponentCore ? "Yes" : "No" );
    }
    else if ( modelType == BaseSimulationModel::COMPONENT )
    {
        res.typeName = "Component Model";

        bool hasComponentCore = isKeywordExist( "EOS" ) || isKeywordExist( "NC" ) || isKeywordExist( "COMPS" ) ||
                                isKeywordExist( "COMPONENT" );

        res.isMatched   = hasComponentCore;
        res.confidence  = res.isMatched ? 1.0f : 0.0f;
        res.judgeReason = QString( "1. Component core keywords exist: %1" ).arg( hasComponentCore ? "Yes" : "No" );
    }
    else
    {
        res = { "Unknown Base Model", true, false, "Invalid base model type", 0.0f };
    }

    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeEnhancedModel( EnhancedSimulationModel modelType )
{
    SingleTypeJudgeResult res;
    res.isJudgable = true;
    res.confidence = 0.0f;

    // Explicit NONE Handling (Clear Logic)
    if ( modelType == EnhancedSimulationModel::NONE )
    {
        res.typeName = "No Enhanced Model";

        bool hasNoEnhancedKey = !isKeywordExist( "THERMAL" ) && !isKeywordExist( "CHEMICAL" ) && !isKeywordExist( "STEAM" ) &&
                                !isKeywordExist( "POLYMER" );

        res.isMatched   = hasNoEnhancedKey;
        res.confidence  = res.isMatched ? 1.0f : 0.0f;
        res.judgeReason = QString( "1. No enhancement keywords detected: %1" ).arg( hasNoEnhancedKey ? "Yes" : "No" );
    }
    else if ( modelType == EnhancedSimulationModel::THERMAL )
    {
        res.typeName = "Thermal Recovery Enhancement";

        bool hasThermalKey = isKeywordExist( "THERMAL" ) || isKeywordExist( "STEAM" ) || isKeywordExist( "SAGD" ) ||
                             isKeywordExist( "TEMPERATURE" );

        res.isMatched   = hasThermalKey;
        res.confidence  = res.isMatched ? 1.0f : 0.0f;
        res.judgeReason = QString( "1. Thermal keywords exist: %1" ).arg( hasThermalKey ? "Yes" : "No" );
    }
    else if ( modelType == EnhancedSimulationModel::CHEMICAL )
    {
        res.typeName = "Chemical Flooding Enhancement";

        bool hasChemicalKey = isKeywordExist( "CHEMICAL" ) || isKeywordExist( "POLYMER" ) || isKeywordExist( "ALKALI" ) ||
                              isKeywordExist( "SURFACTANT" );

        res.isMatched   = hasChemicalKey;
        res.confidence  = res.isMatched ? 1.0f : 0.0f;
        res.judgeReason = QString( "1. Chemical keywords exist: %1" ).arg( hasChemicalKey ? "Yes" : "No" );
    }
    else
    {
        res = { "Unknown Enhanced Model", true, false, "Invalid enhanced model type", 0.0f };
    }

    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeSubModel( SubSimulationModel modelType )
{
    SingleTypeJudgeResult res;
    res.isJudgable = true;
    res.confidence = 0.0f;

    // Low Volatility Oil
    if ( modelType == SubSimulationModel::LOW_VOLATILITY_OIL )
    {
        res.typeName      = "Low Volatility Oil";
        bool hasLowVolKey = isKeywordExist( "LVL" ) || isKeywordExist( "LOWVOL" );
        res.isMatched     = hasLowVolKey;
        res.confidence    = res.isMatched ? 0.8f : 0.0f;
        res.judgeReason   = QString( "1. Low volatility keywords exist: %1" ).arg( hasLowVolKey ? "Yes" : "No" );
    }
    // Regular Waterflooding
    else if ( modelType == SubSimulationModel::REGULAR_WATERFLOOD )
    {
        res.typeName          = "Regular Waterflooding";
        bool hasWaterfloodKey = isKeywordExist( "WATERFLOOD" ) || isKeywordExist( "WATER_INJECTION" );
        res.isMatched         = hasWaterfloodKey;
        res.confidence        = res.isMatched ? 0.7f : 0.0f;
        res.judgeReason       = QString( "1. Waterflooding keywords exist: %1" ).arg( hasWaterfloodKey ? "Yes" : "No" );
    }
    // Composite: Low Volatility + Waterflooding
    else if ( modelType == SubSimulationModel::LOW_VOLATILITY_WATERFLOOD_BO )
    {
        res.typeName          = "Low Volatility Oil + Waterflooding";
        bool hasLowVolKey     = isKeywordExist( "LVL" ) || isKeywordExist( "LOWVOL" );
        bool hasWaterfloodKey = isKeywordExist( "WATERFLOOD" ) || isKeywordExist( "WATER_INJECTION" );

        res.isMatched  = hasLowVolKey && hasWaterfloodKey;
        res.confidence = res.isMatched ? 0.95f : 0.0f; // Higher confidence for composite
        res.judgeReason =
            QString( "1. Low volatility: %1; 2. Waterflooding: %2" ).arg( hasLowVolKey ? "Yes" : "No" ).arg( hasWaterfloodKey ? "Yes" : "No" );
    }
    // Default: Unknown Sub-model
    else
    {
        res = { "Unknown Sub-model", true, false, "Invalid sub-model type", 0.0f };
    }

    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::selectPrimarySubModel( const QMap<SubSimulationModel, SingleTypeJudgeResult>& subResults )
{
    SingleTypeJudgeResult primaryRes    = { "Unknown Sub-model", true, false, "No matched sub-models", 0.0f };
    float                 maxConfidence = 0.0f;

    for ( auto it = subResults.begin(); it != subResults.end(); ++it )
    {
        if ( it.value().confidence > maxConfidence )
        {
            maxConfidence = it.value().confidence;
            primaryRes    = it.value();
        }
    }

    return primaryRes;
}

// -------------------------- Main Judgment Entry --------------------------
ReservoirMultiDimResult ReservoirTypeJudger::judgeFullDimension()
{
    ReservoirMultiDimResult finalResult;

    // 1. Geological Judgment (Placeholders)
    finalResult.structuralResults.insert( StructuralTrapType::ANTICLINE, judgeAnticline() );
    finalResult.structuralResults.insert( StructuralTrapType::FAULT_BLOCK, judgeFaultBlock() );
    finalResult.structuralResults.insert( StructuralTrapType::GAS_CAP, judgeGasCapStructural() );
    finalResult.structuralResults.insert( StructuralTrapType::UNCONFORMITY, judgeUnconformity() );
    finalResult.structuralResults.insert( StructuralTrapType::ONLAP, judgeOnlap() );
    finalResult.structuralResults.insert( StructuralTrapType::SAND_LENS, judgeSandLens() );
    finalResult.structuralResults.insert( StructuralTrapType::LITHOLOGIC_PINCHOUT, judgeLithologicPinchout() );

    finalResult.lithologyResults.insert( LithologyType::UNCONSOLIDATED_SAND, judgeUnconsolidatedSand() );
    finalResult.lithologyResults.insert( LithologyType::NORMAL_SANDSTONE, judgeNormalSandstone() );
    finalResult.lithologyResults.insert( LithologyType::TIGHT_SANDSTONE, judgeTightSandstone() );
    finalResult.lithologyResults.insert( LithologyType::COAL_BED, judgeCoalBed() );
    finalResult.lithologyResults.insert( LithologyType::POROUS_LIMESTONE, judgePorousLimestone() );
    finalResult.lithologyResults.insert( LithologyType::FRACTURED_LIMESTONE, judgeFracturedLimestone() );
    finalResult.lithologyResults.insert( LithologyType::FRACTURED_CARBONATE, judgeFracturedCarbonate() );
    finalResult.lithologyResults.insert( LithologyType::VUGGY_FRACTURED_LIMESTONE, judgeVuggyFracturedLimestone() );
    finalResult.lithologyResults.insert( LithologyType::BIOHERM_BEACH, judgeBiohermBeach() );

    finalResult.specialResults.insert( SpecialMediumType::FRACTURED_SANDSTONE, judgeFracturedSandstone() );
    finalResult.specialResults.insert( SpecialMediumType::FRACTURED_CARBONATE, judgeFracturedCarbonateMedium() );
    finalResult.specialResults.insert( SpecialMediumType::NONE, { "No special medium", true, true, "No fractures/vugs detected", 1.0f } );

    // 2. Base Model Judgment
    SingleTypeJudgeResult boResult = judgeBaseModel( BaseSimulationModel::BLACK_OIL );
    SingleTypeJudgeResult cmResult = judgeBaseModel( BaseSimulationModel::COMPONENT );

    if ( boResult.isMatched )
    {
        finalResult.baseModelResult = boResult;
        m_detectedBaseModel         = BaseSimulationModel::BLACK_OIL;
    }
    else if ( cmResult.isMatched )
    {
        finalResult.baseModelResult = cmResult;
        m_detectedBaseModel         = BaseSimulationModel::COMPONENT;
    }
    else
    {
        finalResult.baseModelResult = { "Unknown Base Model", true, false, "No valid Black Oil/Component keywords", 0.0f };
        m_detectedBaseModel         = BaseSimulationModel::UNKNOWN_BASE;
    }

    // 3. Enhanced Model Judgment
    SingleTypeJudgeResult noneEnhancedResult = judgeEnhancedModel( EnhancedSimulationModel::NONE );
    SingleTypeJudgeResult thermalResult      = judgeEnhancedModel( EnhancedSimulationModel::THERMAL );
    SingleTypeJudgeResult chemicalResult     = judgeEnhancedModel( EnhancedSimulationModel::CHEMICAL );

    finalResult.enhancedModelResults.insert( EnhancedSimulationModel::NONE, noneEnhancedResult );
    finalResult.enhancedModelResults.insert( EnhancedSimulationModel::THERMAL, thermalResult );
    finalResult.enhancedModelResults.insert( EnhancedSimulationModel::CHEMICAL, chemicalResult );

    if ( thermalResult.isMatched )
    {
        finalResult.enhancedModels.insert( EnhancedSimulationModel::THERMAL );
        m_detectedEnhancedModels.insert( EnhancedSimulationModel::THERMAL );
    }
    if ( chemicalResult.isMatched )
    {
        finalResult.enhancedModels.insert( EnhancedSimulationModel::CHEMICAL );
        m_detectedEnhancedModels.insert( EnhancedSimulationModel::CHEMICAL );
    }
    if ( finalResult.enhancedModels.isEmpty() )
    {
        finalResult.enhancedModels.insert( EnhancedSimulationModel::NONE );
        m_detectedEnhancedModels.insert( EnhancedSimulationModel::NONE );
    }

    // 4. Sub-model Judgment (Multi-match + Confidence)
    if ( m_detectedBaseModel == BaseSimulationModel::BLACK_OIL )
    {
        if ( m_detectedEnhancedModels.contains( EnhancedSimulationModel::NONE ) )
        {
            auto res1 = judgeSubModel( SubSimulationModel::REGULAR_BLACK_OIL );
            auto res2 = judgeSubModel( SubSimulationModel::LOW_VOLATILITY_OIL );
            auto res3 = judgeSubModel( SubSimulationModel::REGULAR_WATERFLOOD );
            auto res4 = judgeSubModel( SubSimulationModel::LOW_VOLATILITY_WATERFLOOD_BO );

            if ( res1.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::REGULAR_BLACK_OIL, res1 );
            if ( res2.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::LOW_VOLATILITY_OIL, res2 );
            if ( res3.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::REGULAR_WATERFLOOD, res3 );
            if ( res4.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::LOW_VOLATILITY_WATERFLOOD_BO, res4 );
        }
    }
    else if ( m_detectedBaseModel == BaseSimulationModel::COMPONENT )
    {
        auto res1 = judgeSubModel( SubSimulationModel::CONDENSATE_OIL_CM );
        auto res2 = judgeSubModel( SubSimulationModel::VOLATILE_OIL_CM );
        auto res3 = judgeSubModel( SubSimulationModel::GAS_CAP_OIL_CM );
        auto res4 = judgeSubModel( SubSimulationModel::GAS_INJECTION_CM );

        if ( res1.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::CONDENSATE_OIL_CM, res1 );
        if ( res2.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::VOLATILE_OIL_CM, res2 );
        if ( res3.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::GAS_CAP_OIL_CM, res3 );
        if ( res4.isMatched ) finalResult.subModelResults.insert( SubSimulationModel::GAS_INJECTION_CM, res4 );
    }

    // 5. Select Primary Sub-model (Highest Confidence)
    finalResult.primarySubModelResult = selectPrimarySubModel( finalResult.subModelResults );

    return finalResult;
}

// -------------------------- Geological Judgment Placeholders --------------------------
SingleTypeJudgeResult ReservoirTypeJudger::judgeAnticline()
{
    SingleTypeJudgeResult res;
    res.typeName   = "ANTICLINE" ;
    res.typeName   = QStringLiteral( "Anticlinal Reservoir/Gas Reservoir" );
    res.isJudgable = true;
    res.isMatched  = false;

    if ( !m_rigCaseData )
    {
        res.judgeReason = "Missing rig case data";
        return res;
    }

    QMap<QString, double> depthStats  = calculateGridTopDepthStats();
    double                centerDepth = depthStats.value( "avg", 0.0 );
    double                edgeDepth   = ( depthStats.value( "min", 0.0 ) + depthStats.value( "max", 0.0 ) ) / 2.0;

    bool isAnticline = ( centerDepth - edgeDepth ) < -5.0;

    // For keywords use simplified presence check via existing results
    QString owc                 = getEclipseKeywordValue( QStringLiteral( "OWC" ) );
    QString goc                 = getEclipseKeywordValue( QStringLiteral( "GOC" ) );
    bool    hasUnifiedInterface = !owc.isEmpty() || !goc.isEmpty();

    res.isMatched   = isAnticline && hasUnifiedInterface;
    res.judgeReason = QStringLiteral( "1. Grid top depth characteristics: Central top depth(%1m) < Edge top depth(%2m); 2. Exists unified "
                                      "interface(OWC/GOC): %3" )
                          .arg( centerDepth )
                          .arg( edgeDepth )
                          .arg( hasUnifiedInterface ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeFaultBlock()
{
    SingleTypeJudgeResult res;
    res.typeName      = "FAULT_BLOCK";
    res.typeName   = QStringLiteral( "Fault Block Reservoir/Gas Reservoir" );
    res.isJudgable = true;
    res.isMatched  = false;

    if ( !m_rigCaseData )
    {
        res.judgeReason = "Missing rig case data";
        return res;
    }

    const RigMainGrid* mainGrid = m_rigCaseData->mainGrid();
    bool               hasFault = mainGrid ? !mainGrid->faults().empty() : false;

    RigCaseCellResultsData* cellResults = m_rigCaseData->results( RiaDefines::PorosityModelType::MATRIX_MODEL );
    std::vector<double>     pressureData;
    if ( cellResults )
    {
        RigEclipseResultAddress addr;
        cellResults->maxTimeStepCount( &addr ); // hack to get an address present; we will instead search for PRESSURE

        auto addrs = cellResults->existingResults();
        for ( auto it = addrs.rbegin(); it != addrs.rend(); ++it )
        {
            if ( it->resultName().compare( QStringLiteral( "PRESSURE" ), Qt::CaseInsensitive ) == 0 )
            {
                RigEclipseResultAddress pAddr = *it;
                if ( cellResults->ensureKnownResultLoaded( pAddr ) )
                {
                    auto all = cellResults->cellScalarResults( pAddr );
                    if ( !all.empty() ) pressureData = all[0];
                }
                break;
            }
        }
    }

    const RigActiveCellInfo* activeCellInfo = m_rigCaseData->activeCellInfo( RiaDefines::PorosityModelType::MATRIX_MODEL );

    std::map<double, int> pressureCount;
    for ( size_t i = 0; i < pressureData.size(); ++i )
    {
        if ( activeCellInfo && activeCellInfo->isActive( i ) )
        {
            double pressure = std::round( pressureData[i] * 100.0 ) / 100.0;
            pressureCount[pressure]++;
        }
    }

    bool multiPressureSystem = false;
    if ( pressureCount.size() >= 2 )
    {
        double first        = pressureCount.begin()->first;
        double last         = pressureCount.rbegin()->first;
        multiPressureSystem = ( last - first ) > 5.0;
    }

    res.isMatched   = hasFault && multiPressureSystem;
    res.judgeReason = QStringLiteral( "1. Exists fault definition: %1; 2. Multi-pressure system (Number of pressure values: %2, Pressure "
                                      "difference: %3MPa): %4" )
                          .arg( hasFault ? "Yes" : "No" )
                          .arg( static_cast<int>( pressureCount.size() ) )
                          .arg( pressureCount.size() >= 2 ? ( pressureCount.rbegin()->first - pressureCount.begin()->first ) : 0 )
                          .arg( multiPressureSystem ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeGasCapStructural()
{
    SingleTypeJudgeResult res;
    res.typeName      = "GAS_CAP";
    res.typeName   = "Gas Cap Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QString gocStr   = getEclipseKeywordValue( QStringLiteral( "GOC" ) );
    double  gocDepth = gocStr.toDouble();

    QString rsStr    = getEclipseKeywordValue( QStringLiteral( "RS" ) );
    bool    rsMutate = !rsStr.isEmpty();

    res.isMatched   = ( gocDepth > 0 ) && rsMutate;
    res.judgeReason = QString( "1. GOC (Gas-Oil Contact) depth: %1m; 2. RS (Solution Gas-Oil Ratio) mutates with depth: %2" )
                          .arg( gocDepth )
                          .arg( rsMutate ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeUnconformity()
{
    SingleTypeJudgeResult res;
    res.typeName   = "Unconformity Reservoir/Gas Reservoir";
    res.isJudgable = true;

    if ( !m_rigCaseData )
    {
        res.judgeReason = "Missing rig case data";
        return res;
    }

    RigMainGrid* mainGrid = m_rigCaseData->mainGrid();
    if ( !mainGrid )
    {
        res.judgeReason = "Missing main grid";
        return res;
    }

    int    mutateCount = 0;
    size_t ni          = mainGrid->cellCountI();
    size_t nj          = mainGrid->cellCountJ();

    for ( size_t j = 1; j < nj; ++j )
    {
        for ( size_t i = 1; i < ni; ++i )
        {
            size_t idx1   = mainGrid->cellIndexFromIJK( static_cast<unsigned>( i ), static_cast<unsigned>( j ), 0 );
            size_t idx2   = mainGrid->cellIndexFromIJK( static_cast<unsigned>( i - 1 ), static_cast<unsigned>( j - 1 ), 0 );
            auto   c1     = mainGrid->cellCornerVertices( idx1 );
            auto   c2     = mainGrid->cellCornerVertices( idx2 );
            double depth1 = ( c1[0].z() + c1[1].z() + c1[2].z() + c1[3].z() ) / 4.0;
            double depth2 = ( c2[0].z() + c2[1].z() + c2[2].z() + c2[3].z() ) / 4.0;
            if ( std::abs( depth1 - depth2 ) > 50.0 ) ++mutateCount;
        }
    }

    bool depthMutate = ( mutateCount > 0 );

    QString region          = getEclipseKeywordValue( QStringLiteral( "REGION" ) );
    bool    hasStrataRegion = !region.isEmpty();

    res.isMatched   = depthMutate && hasStrataRegion;
    res.judgeReason = QString( "1. Number of grids with abrupt ZCorn depth change: %1; 2. Strata region (REGION): %2" )
                          .arg( mutateCount )
                          .arg( hasStrataRegion ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeOnlap()
{
    SingleTypeJudgeResult res;
    res.typeName       = "ONLAP";
    res.typeName    = "Onlap Reservoir/Gas Reservoir";
    res.isJudgable  = false;
    res.isMatched   = false;
    res.judgeReason = "[Placeholder] Need to combine geological stratification data + ZCorn 3D morphology analysis. Cannot accurately "
                      "judge with Eclipse keywords only, need to supplement geological model data";
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeSandLens()
{
    SingleTypeJudgeResult res;
    res.typeName      = "SAND_LENS";
    res.typeName   = "Sand Lens Reservoir/Gas Reservoir";
    res.isJudgable = true;

    if ( !m_rigCaseData )
    {
        res.judgeReason = "Missing rig case data";
        return res;
    }

    RigCaseCellResultsData*  cellResults     = m_rigCaseData->results( RiaDefines::PorosityModelType::MATRIX_MODEL );
    const RigActiveCellInfo* activeCellInfo  = cellResults ? cellResults->activeCellInfo() : nullptr;
    int                      activeCellCount = activeCellInfo ? static_cast<int>( activeCellInfo->reservoirActiveCellCount() ) : 0;
    int                      totalCellCount  = static_cast<int>( m_rigCaseData->mainGrid()->totalCellCount() );
    bool                     lensShape       = ( activeCellCount < totalCellCount * 0.3 );

    QMap<QString, double> porPerm   = calculatePorPermAvg();
    double                edgePermX = 0.0;
    bool                  permDrop  = ( porPerm["permX"] > 10.0 ) && ( edgePermX < 0.1 );

    res.isMatched   = lensShape && permDrop;
    res.judgeReason = QString( "1. Active grid ratio: %1% (<30% for lens shape); 2. PERMX drops sharply at boundary: %2" )
                          .arg( (double)activeCellCount / totalCellCount * 100 )
                          .arg( permDrop ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeLithologicPinchout()
{
    SingleTypeJudgeResult res;
    res.typeName       = "LITHOLOGIC_PINCHOUT";
    res.typeName    = "Lithologic Pinchout Reservoir/Gas Reservoir";
    res.isJudgable  = false;
    res.isMatched   = false;
    res.judgeReason = "[Placeholder] Need to count PERMX/PORO decreasing trend along sedimentary direction, require custom grid traversal "
                      "logic + sedimentary facies data, complex logic";
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeUnconsolidatedSand()
{
    SingleTypeJudgeResult res;
    res.typeName      = "UNCONSOLIDATED_SAND";
    res.typeName   = "Unconsolidated Sand Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QMap<QString, double> porPerm  = calculatePorPermAvg();
    double                compress = getEclipseKeywordValue( QStringLiteral( "COMPRESS" ) ).toDouble();
    QString               lith     = getEclipseKeywordValue( QStringLiteral( "LITH" ) );

    bool isUnconsolidated = ( porPerm["poro"] > 0.3 ) && ( compress > 5e-4 ) && lith.contains( "UNCONSOL", Qt::CaseInsensitive );

    res.isMatched   = isUnconsolidated;
    res.judgeReason = QString( "1. Average porosity: %1% (>30%); 2. Rock compressibility: %2 MPa⁻¹ (>5e-4); 3. Lithology label: %3" )
                          .arg( porPerm["poro"] * 100 )
                          .arg( compress )
                          .arg( lith );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeNormalSandstone()
{
    SingleTypeJudgeResult res;
    res.typeName      = "NORMAL_SANDSTONE";
    res.typeName   = "Normal Sandstone Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QMap<QString, double> porPerm = calculatePorPermAvg();
    QString               lith    = getEclipseKeywordValue( QStringLiteral( "LITH" ) );

    bool isNormal = ( porPerm["poro"] >= 0.15 && porPerm["poro"] <= 0.3 ) && ( porPerm["permX"] >= 10.0 && porPerm["permX"] <= 1000.0 ) &&
                    lith.contains( "SAND", Qt::CaseInsensitive );

    res.isMatched   = isNormal;
    res.judgeReason = QString( "1. Average porosity: %1% (15%-30%); 2. Average PERMX: %2 mD (10-1000); 3. Lithology label: %3" )
                          .arg( porPerm["poro"] * 100 )
                          .arg( porPerm["permX"] )
                          .arg( lith );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeTightSandstone()
{
    SingleTypeJudgeResult res;
    res.typeName      = "TIGHT_SANDSTONE";
    res.typeName   = "Tight Sandstone Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QMap<QString, double> porPerm = calculatePorPermAvg();
    QString               fract   = getEclipseKeywordValue( QStringLiteral( "FRACT" ) );

    bool isTight = ( porPerm["permX"] < 0.1 ) && ( porPerm["poro"] < 0.1 ) && !fract.isEmpty();

    res.isMatched   = isTight;
    res.judgeReason = QString( "1. Average PERMX: %1 mD (<0.1); 2. Average porosity: %2% (<10%); 3. Fracturing parameter (FRACT): %3" )
                          .arg( porPerm["permX"] )
                          .arg( porPerm["poro"] * 100 )
                          .arg( fract.isEmpty() ? "None" : "Exists" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeCoalBed()
{
    SingleTypeJudgeResult res;
    res.typeName      = "COAL_BED_GAS";
    res.typeName   = "Coal Bed Gas Reservoir";
    res.isJudgable = true;

    QString lith  = getEclipseKeywordValue( QStringLiteral( "LITH" ) );
    QString cleat = getEclipseKeywordValue( QStringLiteral( "CLEAT" ) );

    bool isCoal = lith.contains( "COAL", Qt::CaseInsensitive ) && !cleat.isEmpty();

    res.isMatched   = isCoal;
    res.judgeReason = QString( "1. Lithology (LITH) labeled as coal: %1; 2. Cleat permeability (CLEAT) parameter: %2" )
                          .arg( lith.contains( "COAL" ) ? "Yes" : "No" )
                          .arg( cleat.isEmpty() ? "None" : "Exists" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgePorousLimestone()
{
    SingleTypeJudgeResult res;
    res.typeName   = "Porous Limestone Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QMap<QString, double> porPerm = calculatePorPermAvg();
    QString               lith    = getEclipseKeywordValue( QStringLiteral( "LITH" ) );
    QString               dfnum   = getEclipseKeywordValue( QStringLiteral( "DFNUM" ) );

    bool isPorousLimestone = ( lith.contains( "LIM", Qt::CaseInsensitive ) ) && ( porPerm["poro"] >= 0.05 && porPerm["poro"] <= 0.2 ) &&
                             ( porPerm["permX"] >= 1.0 && porPerm["permX"] <= 100.0 ) && dfnum.isEmpty();

    res.isMatched = isPorousLimestone;
    res.judgeReason =
        QString( "1. Lithology is limestone: %1; 2. Porosity: %2% (5%-20%); 3. PERMX: %3 mD (1-100); 4. No fracture parameter (DFNUM): %4" )
            .arg( lith.contains( "LIM" ) ? "Yes" : "No" )
            .arg( porPerm["poro"] * 100 )
            .arg( porPerm["permX"] )
            .arg( dfnum.isEmpty() ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeFracturedLimestone()
{
    SingleTypeJudgeResult res;
    res.typeName   = "Fractured Limestone Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QString lith  = getEclipseKeywordValue( QStringLiteral( "LITH" ) );
    QString dfnum = getEclipseKeywordValue( QStringLiteral( "DFNUM" ) );
    QString dual  = getEclipseKeywordValue( QStringLiteral( "DUAL" ) );

    bool isFracturedLimestone = lith.contains( "LIM", Qt::CaseInsensitive ) && ( !dfnum.isEmpty() || !dual.isEmpty() );

    res.isMatched   = isFracturedLimestone;
    res.judgeReason = QString( "1. Lithology is limestone: %1; 2. Exists fracture parameters (DFNUM/DUAL): %2" )
                          .arg( lith.contains( "LIM" ) ? "Yes" : "No" )
                          .arg( ( !dfnum.isEmpty() || !dual.isEmpty() ) ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeFracturedCarbonate()
{
    SingleTypeJudgeResult res;
    res.typeName   = "Fractured Carbonate Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QString lith        = getEclipseKeywordValue( QStringLiteral( "LITH" ) );
    bool    isCarbonate = lith.contains( "CARB", Qt::CaseInsensitive ) || lith.contains( "LIM", Qt::CaseInsensitive );

    QString dfnum       = getEclipseKeywordValue( QStringLiteral( "DFNUM" ) );
    QString fracp       = getEclipseKeywordValue( QStringLiteral( "FRACP" ) );
    bool    hasFracture = !dfnum.isEmpty() && ( fracp.toDouble() > 0.0 );

    res.isMatched   = isCarbonate && hasFracture;
    res.judgeReason = QString( "1. Lithology is carbonate: %1; 2. Fracture parameter (DFNUM) exists and FRACP(%2)>0: %3" )
                          .arg( isCarbonate ? "Yes" : "No" )
                          .arg( fracp.toDouble() )
                          .arg( hasFracture ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeVuggyFracturedLimestone()
{
    SingleTypeJudgeResult res;
    res.typeName    = "Vuggy Fractured Limestone Reservoir/Gas Reservoir";
    res.isJudgable  = false;
    res.isMatched   = false;
    res.judgeReason = "[Placeholder] Need to parse CAVITY (vug volume) + triple medium parameters. ResInsight native API does not directly "
                      "encapsulate this, need to custom parse vug parameters in DUAL module";
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeBiohermBeach()
{
    SingleTypeJudgeResult res;
    res.typeName    = "Bioherm/Beach Reservoir/Gas Reservoir";
    res.isJudgable  = false;
    res.isMatched   = false;
    res.judgeReason = "[Placeholder] Can only judge general category by LITH label. Microscopic reef morphology needs to combine "
                      "geological facies belt data, cannot be accurately judged by Eclipse files alone";
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeFracturedSandstone()
{
    SingleTypeJudgeResult res;
    res.typeName   = "Fractured Sandstone Reservoir/Gas Reservoir";
    res.isJudgable = true;

    QString lith        = getEclipseKeywordValue( QStringLiteral( "LITH" ) );
    bool    isSandstone = lith.contains( "SAND", Qt::CaseInsensitive );

    QString dfnum            = getEclipseKeywordValue( QStringLiteral( "DFNUM" ) );
    QString permfr           = getEclipseKeywordValue( QStringLiteral( "PERMFR" ) );
    QString dual             = getEclipseKeywordValue( QStringLiteral( "DUAL" ) );
    bool    hasFractureParam = !dfnum.isEmpty() || !permfr.isEmpty() || !dual.isEmpty();

    res.isMatched   = isSandstone && hasFractureParam;
    res.judgeReason = QString( "1. Lithology (LITH) is sandstone: %1; 2. Exists fracture parameters (DFNUM/PERMFR/DUAL): %2" )
                          .arg( isSandstone ? "Yes" : "No" )
                          .arg( hasFractureParam ? "Yes" : "No" );
    return res;
}

SingleTypeJudgeResult ReservoirTypeJudger::judgeFracturedCarbonateMedium()
{
    SingleTypeJudgeResult res;
    res.typeName   = "Fractured Carbonate";
    res.isJudgable = true;
    res.confidence = 0.0f;
    res.isMatched  = false;

    // 防护：核心数据为空时直接返回未匹配
    if ( !m_rigCaseData )
    {
        res.judgeReason = "RigEclipseCaseData is null (no reservoir data available)";
        return res;
    }

    // -------------------------- 核心判断逻辑（裂缝性碳酸盐岩判定依据） --------------------------
    // 维度1：关键字匹配（碳酸盐岩+裂缝相关关键字）
    bool hasCarbonateKeywords = isKeywordExist( "CARBONATE" ) || isKeywordExist( "LIMESTONE" ) || isKeywordExist( "DOLOMITE" ) ||
                                isKeywordExist( "CALCITE" );
    bool hasFractureKeywords = isKeywordExist( "FRAC" ) || isKeywordExist( "FRACTURE" ) || isKeywordExist( "VUG" ) ||
                               isKeywordExist( "FRAC_PERM" ); // 裂缝渗透率

    // 维度2：物性参数验证（碳酸盐岩典型物性特征：低基质孔、裂缝发育区渗透率突变）
    QMap<QString, double> porPermAvg = calculatePorPermAvg();
    double                avgPoro    = porPermAvg.value( "poro", 0.0 );
    double                avgPermX   = porPermAvg.value( "permX", 0.0 );
    // 碳酸盐岩判定阈值：孔隙度<10%（低孔），渗透率>10mD（裂缝贡献）（油气藏工程通用阈值）
    bool isCarbonatePorPerm = ( avgPoro < 0.10 ) && ( avgPermX > 10.0 );

    // 维度3：ResInsight 裂缝模型验证（是否启用裂缝孔隙度模型）
    RigCaseCellResultsData* fractureResults      = m_rigCaseData->results( RiaDefines::PorosityModelType::FRACTURE_MODEL );
    bool                    hasFractureModelData = ( fractureResults != nullptr ) && !fractureResults->existingResults().empty();

    // -------------------------- 结果判定与置信度计算 --------------------------
    // 核心匹配条件：关键字匹配 + 物性匹配 或 关键字匹配 + 裂缝模型存在
    res.isMatched = ( hasCarbonateKeywords && hasFractureKeywords && isCarbonatePorPerm ) ||
                    ( hasCarbonateKeywords && hasFractureKeywords && hasFractureModelData );

    // 置信度分级（贴合油气藏工程判断逻辑）
    if ( res.isMatched )
    {
        if ( hasCarbonateKeywords && hasFractureKeywords && isCarbonatePorPerm && hasFractureModelData )
        {
            res.confidence = 0.95f; // 多维度验证通过，高置信度
        }
        else if ( hasCarbonateKeywords && hasFractureKeywords && ( isCarbonatePorPerm || hasFractureModelData ) )
        {
            res.confidence = 0.80f; // 核心维度验证通过，中高置信度
        }
    }

    // -------------------------- 判定理由拼接（便于调试/日志） --------------------------
    res.judgeReason = QString( "1. Carbonate keywords exist: %1; "
                               "2. Fracture keywords exist: %2; "
                               "3. Carbonate typical poro/perm (poro<10%, perm>10mD): %3 (avg poro: %4, avg perm: %5 mD); "
                               "4. Fracture model data exists: %6" )
                          .arg( hasCarbonateKeywords ? "Yes" : "No" )
                          .arg( hasFractureKeywords ? "Yes" : "No" )
                          .arg( isCarbonatePorPerm ? "Yes" : "No" )
                          .arg( QString::number( avgPoro, 'f', 3 ) ) // 保留3位小数
                          .arg( QString::number( avgPermX, 'f', 1 ) )
                          .arg( hasFractureModelData ? "Yes" : "No" );

    return res;
}