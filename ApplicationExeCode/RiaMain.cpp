/////////////////////////////////////////////////////////////////////////////////
//
//  Copyright (C) 2011-2012 Statoil ASA, Ceetron AS
//
//  ResInsight is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  ResInsight is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or
//  FITNESS FOR A PARTICULAR PURPOSE.
//
//  See the GNU General Public License at <http://www.gnu.org/licenses/gpl.html>
//  for more details.
//
/////////////////////////////////////////////////////////////////////////////////

#include "RiaArgumentParser.h"
#include "RiaMainTools.h"
#include "RiaPreferences.h"
#include "RiaQuantityInfoTools.h"

#ifdef ENABLE_GRPC
#include "RiaGrpcConsoleApplication.h"
#include "RiaGrpcGuiApplication.h"
#else
#include "RiaConsoleApplication.h"
#include "RiaGuiApplication.h"
#endif

#include "cafUiAppearanceSettings.h"

#include "cvfProgramOptions.h"
#include "cvfqtUtils.h"

#include <QFile>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QCryptographicHash>
#include <QHostInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QDir>

#ifndef WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

#include <signal.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#pragma comment(lib, "wbemuuid.lib")
#endif

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/ec.h>
#include <openssl/applink.c> // Fix for OPENSSL_Uplink error on Windows
#include <fstream>
#include <vector>
#include <mutex>

static EVP_PKEY*  g_publicKey = nullptr;
static std::string g_strPublicKey = R"(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEqynoL+VmxNKfUjtKqOMJMW7bciL9
RIzAvFppgOOWCy5ZGCTFEnYisNEEno8bibfdJgIxWIKvjm/IIBlNrOgf4Q==
-----END PUBLIC KEY-----)";
                     
static std::mutex g_keyMutex;

void manageSegFailure( int signalCode );

RiaApplication* createApplication( int& argc, char* argv[] )
{
    for ( int i = 1; i < argc; ++i )
    {
        if ( !qstrcmp( argv[i], "--console" ) || !qstrcmp( argv[i], "--unittest" ) )
        {
#ifdef ENABLE_GRPC
            return new RiaGrpcConsoleApplication( argc, argv );
#else
            return new RiaConsoleApplication( argc, argv );
#endif
        }
    }
#ifdef ENABLE_GRPC
    return new RiaGrpcGuiApplication( argc, argv );
#else
    return new RiaGuiApplication( argc, argv );
#endif
}

static QString computeMachineCode()
{
#ifdef Q_OS_WIN
    // Use WMI to query Win32_NetworkAdapter PermanentAddress for reliable physical MAC
    QString hwAddr;
    HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    bool coInitialized = SUCCEEDED(hres);
    if (SUCCEEDED(hres))
    {
        hres = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                                   RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
        IWbemLocator* pLoc = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator,0, CLSCTX_INPROC_SERVER,
                                       IID_IWbemLocator, (LPVOID*)&pLoc)))
        {
            IWbemServices* pSvc = nullptr;
            if (SUCCEEDED(pLoc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), NULL, NULL,0, NULL,0,0, &pSvc)))
            {
                CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                                  RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);

                IEnumWbemClassObject* pEnumerator = nullptr;
                if (SUCCEEDED(pSvc->ExecQuery(_bstr_t(L"WQL"),
                                              _bstr_t(L"SELECT PermanentAddress FROM Win32_NetworkAdapter WHERE PermanentAddress IS NOT NULL"),
                                              WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnumerator)))
                {
                    IWbemClassObject* pclsObj = nullptr;
                    ULONG uReturn =0;
                    while (pEnumerator && SUCCEEDED(pEnumerator->Next(WBEM_INFINITE,1, &pclsObj, &uReturn)) && uReturn)
                    {
                        VARIANT vtProp;
                        VariantInit(&vtProp);
                        if (SUCCEEDED(pclsObj->Get(_bstr_t(L"PermanentAddress"),0, &vtProp, NULL, NULL)))
                        {
                            if (vtProp.vt == VT_BSTR && vtProp.bstrVal)
                            {
                                QString candidate = QString::fromWCharArray(vtProp.bstrVal).trimmed();
                                if (!candidate.isEmpty() && candidate != "00:00:00:00:00:00")
                                {
                                    hwAddr = candidate;
                                    VariantClear(&vtProp);
                                    pclsObj->Release();
                                    break;
                                }
                            }
                            VariantClear(&vtProp);
                        }
                        pclsObj->Release();
                    }
                    pEnumerator->Release();
                }
                pSvc->Release();
            }
            pLoc->Release();
        }
        if (coInitialized)
            CoUninitialize();
    }

    if (!hwAddr.isEmpty())
    {
        const QByteArray hash = QCryptographicHash::hash(hwAddr.toLatin1(), QCryptographicHash::Sha256);
        return QString::fromLatin1(hash.toHex().left(12).toUpper());
    }
    // Fallback to previous method if WMI didn't yield a physical MAC
#endif // Q_OS_WIN

#ifndef Q_OS_WIN
    // Try to read permanent hardware address from /sys/class/net/<iface>/perm_addr
    QString hwAddr;
    QDir sysNetDir("/sys/class/net");
    if (sysNetDir.exists())
    {
        const QStringList ifaces = sysNetDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& iface : ifaces)
        {
            QString permPath = sysNetDir.filePath(iface + "/perm_addr");
            QFile f(permPath);
            if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text))
            {
                QByteArray data = f.readAll();
                f.close();
                QString candidate = QString::fromLatin1(data).trimmed();
                if (!candidate.isEmpty() && candidate != "00:00:00:00:00:00")
                {
                    hwAddr = candidate;
                    break;
                }
            }
        }
    }

    if (!hwAddr.isEmpty())
    {
        const QByteArray hash = QCryptographicHash::hash(hwAddr.toLatin1(), QCryptographicHash::Sha256);
        return QString::fromLatin1(hash.toHex().left(12).toUpper());
    }
#endif // !Q_OS_WIN

    // Previous fallback: try QNetworkInterface list and then hostname
    auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces)
    {
        const QByteArray hw = iface.hardwareAddress().toLatin1();
        if (!hw.isEmpty() && hw != "00:00:00:00:00:00")
        {
            const QByteArray hash = QCryptographicHash::hash(hw, QCryptographicHash::Sha256);
            return QString::fromLatin1(hash.toHex().left(12).toUpper());
        }
    }

    // Fallback: use hostname
    QString host = QHostInfo::localHostName();
    const QByteArray hash = QCryptographicHash::hash(host.toLatin1(), QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.toHex().left(12).toUpper());
}


EVP_PKEY* string_to_evp_pkey( std::string key_str, int is_private )
{
    OPENSSL_init_crypto( OPENSSL_INIT_LOAD_CRYPTO_STRINGS | OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS,
                         NULL );
    if ( key_str == "" || key_str.length() == 0 )
    {
        fprintf( stderr, "empty key\n" );
        return NULL;
    }

    BIO* bio = BIO_new_mem_buf( key_str.c_str(), -1 );
    if ( bio == NULL )
    {
        fprintf( stderr, "create BIO failed\n" );
        return NULL;
    }

    EVP_PKEY* pkey = NULL;
    if ( is_private )
    {
        pkey = PEM_read_bio_PrivateKey( bio, NULL, NULL, NULL );
        if ( pkey == NULL )
        {
            fprintf( stderr, "½âÎöË½Ô¿Ê§°Ü\n" );
        }
    }
    else
    {
        pkey = PEM_read_bio_PUBKEY( bio, NULL, NULL, NULL );
        if ( pkey == NULL )
        {
            fprintf( stderr, "½âÎö¹«Ô¿Ê§°Ü\n" );
        }

        BIO_free( bio );

        return pkey;
    }
}

static std::string base64Encode( const std::vector<unsigned char>& data )
{
    BIO* bmem = BIO_new( BIO_s_mem() );
    BIO* b64  = BIO_new( BIO_f_base64() );
    b64       = BIO_push( b64, bmem );
    BIO_set_flags( b64, BIO_FLAGS_BASE64_NO_NL );
    BIO_write( b64, data.data(), static_cast<int>( data.size() ) );
    BIO_flush( b64 );
    BUF_MEM* bptr;
    BIO_get_mem_ptr( b64, &bptr );
    std::string ret( bptr->data, bptr->length );
    BIO_free_all( b64 );
    return ret;
}

static std::vector<unsigned char> base64Decode( const std::string& in )
{
    BIO* b64  = BIO_new( BIO_f_base64() );
    BIO* bmem = BIO_new_mem_buf( in.data(), static_cast<int>( in.size() ) );
    bmem      = BIO_push( b64, bmem );
    BIO_set_flags( bmem, BIO_FLAGS_BASE64_NO_NL );
    std::vector<unsigned char> out( in.size() );
    int                        len = BIO_read( bmem, out.data(), static_cast<int>( out.size() ) );
    if ( len <= 0 )
    {
        BIO_free_all( bmem );
        return {};
    }
    out.resize( len );
    BIO_free_all( bmem );
    return out;
}

bool verifyJsonString( const std::string& jsonStr, const std::string& base64Signature )
{
    std::lock_guard<std::mutex> lock( g_keyMutex );
    g_publicKey = string_to_evp_pkey( g_strPublicKey,false );
    if ( !g_publicKey ) return false;

    auto sig = base64Decode( base64Signature );
    if ( sig.empty() ) return false;

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if ( !mdctx ) return false;

    const EVP_MD* md = EVP_sha256();
    int           rc = -1;

    if ( EVP_DigestVerifyInit( mdctx, nullptr, md, nullptr, g_publicKey ) <= 0 ) goto err;
    if ( EVP_DigestVerifyUpdate( mdctx, jsonStr.data(), jsonStr.size() ) <= 0 ) goto err;

    rc = EVP_DigestVerifyFinal( mdctx, sig.data(), sig.size() );
    EVP_MD_CTX_free( mdctx );
    return rc == 1;

err:
    EVP_MD_CTX_free( mdctx );
    return false;
}

bool verifyJsonFile( const std::string& filePath, const std::string& machineCode, bool& isValid )
{
    isValid = false;
    
    std::ifstream ifs( filePath );
    if ( !ifs.is_open() ) return false;

    nlohmann::json j;
    try
    {
        ifs >> j;
    }
    catch ( ... )
    {
        return false;
    }

    if ( !j.contains( "mac" ) || !j.contains( "overdue" ) || !j.contains( "function" ) || !j.contains( "signature" ) )
    {
        return false;
    }

    nlohmann::json core = { { "mac", j["mac"] }, { "overdue", j["overdue"] }, { "function", j["function"] } };

    std::string coreStr   = core.dump();
    std::string signature = j["signature"].get<std::string>();

    isValid = j["mac"].get<std::string>() == machineCode && verifyJsonString( coreStr, signature );
    return true;
}

bool verifyMachineCode(const QString& keyFilePath, const QString& machineCode)
{

}

// Helper stub for public key validation. The real verification algorithm will be provided later.
static bool validatePublicKey( const QString& keyFilePath, const QString& machineCode )
{
    // Placeholder implementation:
    // - Open the file and perform any parsing and cryptographic checks here when algorithm is available.
    // - For now, return false to indicate validation not implemented.
    bool bRet = false;
    bool isValid = false;
    Q_UNUSED( keyFilePath );
    Q_UNUSED( machineCode );
    bRet = verifyJsonFile( keyFilePath.toStdString(), machineCode.toStdString(), isValid );

    return isValid;
}

// Shows a blocking dialog with machine code and a password QLineEdit that only accepts alphanumeric.
// Behavior changed to the following:
//1) Check for public key file named "key.json" in application directory (and current working directory).
//2) If not present: show dialog with machine code and instructions to request a trial -> return false (main will exit).
///3) If present: attempt to validate via validatePublicKey(). If valid -> return true (continue startup).
// If invalid -> show dialog informing that public key is incorrect -> return false (main will exit).
static bool showMachineCodeAndRequirePassword()
{
    const QString keyFileName = QStringLiteral("key.json");
    QString machineCode = computeMachineCode();

    // Search for key file in application directory and current working directory
    QString appDirPath = QCoreApplication::applicationDirPath();
    QString cwdPath = QDir::currentPath();

    QStringList candidatePaths;
    candidatePaths << QDir(appDirPath).filePath(keyFileName);
    if (cwdPath != appDirPath)
        candidatePaths << QDir(cwdPath).filePath(keyFileName);

    QString foundKeyPath;
    for ( const QString& p : candidatePaths )
    {
        QFile f(p);
        if ( f.exists() )
        {
            foundKeyPath = p;
            break;
        }
    }

    if ( foundKeyPath.isEmpty() )
    {
        // No public key found -> show machine code and instruct to request trial, then exit.
        QDialog dlg;
        dlg.setWindowTitle( "License verification" );
        dlg.setModal( true );

        QVBoxLayout* layout = new QVBoxLayout( &dlg );

        QLabel* infoLabel = new QLabel( QStringLiteral( "No license key found. This machine code is %1. Please contact the licensor to obtain a key or request a trial." ).arg( machineCode ) );
        infoLabel->setWordWrap( true );
        layout->addWidget( infoLabel );

        QDialogButtonBox* buttons = new QDialogButtonBox( QDialogButtonBox::Ok, &dlg );
        layout->addWidget( buttons );

        QObject::connect( buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept );

        dlg.exec();
        return false;
    }

    // Key file found -> attempt validation. The real algorithm will be implemented later.
    bool valid = validatePublicKey(foundKeyPath, machineCode);
    if ( valid )
    {
        // Key validated - continue startup
        return true;
    }

    // Key present but invalid -> notify user and exit
    QDialog errDlg;
    errDlg.setWindowTitle( "License verification" );
    errDlg.setModal( true );

    QVBoxLayout* layout = new QVBoxLayout( &errDlg );
    QLabel* errLabel = new QLabel( QStringLiteral( "The provided public key appears to be invalid for this machine. Please contact the licensor or request a trial. Machine code: %1" ).arg( machineCode ) );
    errLabel->setWordWrap( true );
    layout->addWidget( errLabel );

    QDialogButtonBox* buttons = new QDialogButtonBox( QDialogButtonBox::Ok, &errDlg );
    layout->addWidget( buttons );
    QObject::connect( buttons, &QDialogButtonBox::accepted, &errDlg, &QDialog::accept );

    errDlg.exec();
    return false;
}

int main( int argc, char* argv[] )
{
#ifndef WIN32
    // From Qt5.3 and onwards Qt has a mechanism for checking this automatically
    // But it only checks user id not group id, so better to do it ourselves.
    if ( getuid() != geteuid() || getgid() != getegid() )
    {
        std::cerr << "FATAL: The application binary appears to be running setuid or setgid, this is a security hole."
                  << std::endl;
        return 1;
    }
#endif

    RiaMainTools::deleteStaleSettingsLockFiles();

    // The Qt::AA_ShareOpenGLContexts setting is needed when we have multiple viz widgets in flight
    // and we have a setup where these widgets belong to different top-level windows, or end up
    // belonging to different top-level windows through re-parenting.
    // See test application QtTestBenchOpenGLWidget
    QApplication::setAttribute( Qt::AA_ShareOpenGLContexts );

    // Enable Qt high-DPI support before creating the application object
    QApplication::setAttribute( Qt::AA_EnableHighDpiScaling );
    QApplication::setAttribute( Qt::AA_UseHighDpiPixmaps );

#ifdef Q_OS_WIN
    // Try to set per-monitor DPI awareness on Windows.
    // Prefer SetProcessDpiAwarenessContext (Win10+). If not available, fall back to SetProcessDpiAwareness from shcore.dll.
    HMODULE user32 = LoadLibraryA("user32.dll");
    if ( user32 )
    {
        typedef BOOL( WINAPI* SetProcessDpiAwarenessContext_t )( HANDLE );
        auto spdac = reinterpret_cast<SetProcessDpiAwarenessContext_t>( GetProcAddress( user32, "SetProcessDpiAwarenessContext" ) );
        if ( spdac )
        {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is not defined on older SDKs; use (HANDLE)-4 which is the documented value
            spdac( reinterpret_cast<HANDLE>( -4 ) );
        }
        else
        {
            HMODULE shcore = LoadLibraryA("shcore.dll");
            if ( shcore )
            {
                typedef HRESULT( WINAPI* SetProcessDpiAwareness_t )( int );
                auto spda = reinterpret_cast<SetProcessDpiAwareness_t>( GetProcAddress( shcore, "SetProcessDpiAwareness" ) );
                if ( spda )
                {
                    // PROCESS_PER_MONITOR_DPI_AWARE = 2
                    spda( 2 );
                }
                FreeLibrary( shcore );
            }
        }
        FreeLibrary( user32 );
    }
#endif

    // Create feature manager before the application object is created
    RiaMainTools::initializeSingletons();
    RiaQuantityInfoTools::initializeSummaryKeywords();

    // https://www.w3.org/wiki/CSS/Properties/color/keywords
    caf::UiAppearanceSettings::instance()->setAutoValueEditorColor( "moccasin" );

    std::unique_ptr<RiaApplication> app( createApplication( argc, argv ) );

    // Show machine code dialog before GUI is initialized (only for GUI app)
    if ( dynamic_cast<RiaGuiApplication*>( app.get() ) != nullptr )
    {
        bool ok = showMachineCodeAndRequirePassword();
        if ( !ok )
        {
            // User cancelled or didn't provide password / key invalid -> exit
            return 0;
        }
    }

    cvf::ProgramOptions progOpt;
    bool                result = RiaArgumentParser::parseArguments( &progOpt );

    const cvf::String usageText = progOpt.usageText( 110, 30 );
    app->initialize();
    app->setCommandLineHelpText( cvfqt::Utils::toQString( usageText ) );

    if ( !result )
    {
        std::vector<cvf::String> unknownOptions = progOpt.unknownOptions();
        QString                  unknownOptionsText;
        for ( cvf::String option : unknownOptions )
        {
            unknownOptionsText += QString( "\tUnknown option: %1\n" ).arg( cvfqt::Utils::toQString( option ) );
        }

        app->showFormattedTextInMessageBoxOrConsole(
            "ERROR: Unknown command line options detected ! \n" + unknownOptionsText + "\n\n" +
            "The current command line options in ResInsight are:\n" + app->commandLineParameterHelp() );

        if ( dynamic_cast<RiaGuiApplication*>( app.get() ) == nullptr )
        {
            return 1;
        }
    }

    QLocale::setDefault( QLocale( QLocale::English, QLocale::UnitedStates ) );
    setlocale( LC_NUMERIC, "C" );

    // Set up signal handlers
    signal( SIGINT, manageSegFailure );
    signal( SIGILL, manageSegFailure );
    signal( SIGFPE, manageSegFailure );
    signal( SIGSEGV, manageSegFailure );
    signal( SIGTERM, manageSegFailure );
    signal( SIGABRT, manageSegFailure );

    // Handle the command line arguments.
    // Todo: Move to a one-shot timer, delaying the execution until we are inside the event loop.
    // The complete handling of the resulting ApplicationStatus must be moved along.
    // The reason for this is: deleteLater() does not work outside the event loop
    // Make execution of command line stuff operate in identical conditions as interactive operation.

    RiaApplication::ApplicationStatus status = app->handleArguments( &progOpt );

    if ( status == RiaApplication::ApplicationStatus::EXIT_COMPLETED )
    {
        // Make sure project is closed to avoid assert and crash in destruction of widgets
        app->closeProject();

        app.reset();
        RiaMainTools::releaseSingletonAndFactoryObjects();

        return 0;
    }
    else if ( status == RiaApplication::ApplicationStatus::EXIT_WITH_ERROR )
    {
        // Make sure project is closed to avoid assert and crash in destruction of widgets
        app->closeProject();

        return 2;
    }
    else if ( status == RiaApplication::ApplicationStatus::KEEP_GOING )
    {
        int exitCode = 0;
        try
        {
#ifdef ENABLE_GRPC
            auto grpcInterface = dynamic_cast<RiaGrpcApplicationInterface*>( app.get() );
            if ( grpcInterface && grpcInterface->initializeGrpcServer( progOpt ) )
            {
                grpcInterface->launchGrpcServer();

                if ( cvf::Option o = progOpt.option( "portnumberfile" ) )
                {
                    if ( o.valueCount() == 1 )
                    {
                        int     portNumber = grpcInterface->portNumber();
                        QString fileName   = QString::fromStdString( o.value( 0 ).toStdString() );

                        // Write port number to the file given file.
                        // Temp file is used to avoid incomplete reads.
                        QString tempFilePath = fileName + ".tmp";
                        QFile   file( tempFilePath );
                        if ( file.open( QIODevice::WriteOnly | QIODevice::Text ) )
                        {
                            QTextStream out( &file );
                            out << portNumber << "\n";
                        }
                        file.close();

                        QFile::rename( tempFilePath, fileName );
                    }
                }
            }
#endif
            // Check if max thread count is set either from command line or preferences
            app->setThreadCount();

            exitCode = QCoreApplication::instance()->exec();
        }
        catch ( std::exception& exep )
        {
            std::cout << "A standard c++ exception that terminated ResInsight caught in RiaMain.cpp: " << exep.what()
                      << std::endl;
            throw;
        }
        catch ( ... )
        {
            std::cout << "An unknown exception that terminated ResInsight caught in RiaMain.cpp.  " << std::endl;
            throw;
        }

        app.reset();
        RiaMainTools::releaseSingletonAndFactoryObjects();

        return exitCode;
    }

    CVF_ASSERT( false && "Unknown ApplicationStatus" );
    return -1;
}
