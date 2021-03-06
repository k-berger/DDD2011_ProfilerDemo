// CodeInjection.cpp : Implementation of CCodeInjection
#include "stdafx.h"
#include "CodeInjection.h"

#include "Method.h"

// CCodeInjection
HRESULT STDMETHODCALLTYPE CCodeInjection::Initialize( 
    /* [in] */ IUnknown *pICorProfilerInfoUnk) 
{
    OLECHAR szGuid[40]={0};
    int nCount = ::StringFromGUID2(CLSID_CodeInjection, szGuid, 40);
    ATLTRACE(_T("::Initialize - %s"), W2CT(szGuid));
    
    m_profilerInfo3 = pICorProfilerInfoUnk;
    if (m_profilerInfo3 == NULL) return E_FAIL;

    DWORD dwMask = 0;
    dwMask |= COR_PRF_MONITOR_MODULE_LOADS;			// Controls the ModuleLoad, ModuleUnload, and ModuleAttachedToAssembly callbacks.
    dwMask |= COR_PRF_MONITOR_JIT_COMPILATION;	    // Controls the JITCompilation, JITFunctionPitched, and JITInlining callbacks.
    dwMask |= COR_PRF_DISABLE_INLINING;				// Disables all inlining.
    dwMask |= COR_PRF_DISABLE_OPTIMIZATIONS;		// Disables all code optimizations.

    m_profilerInfo3->SetEventMask(dwMask);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE CCodeInjection::Shutdown( void) 
{
    ATLTRACE(_T("::Shutdown"));
    return S_OK;
}

HRESULT CCodeInjection::GetInjectedRef(ModuleID moduleId, mdModuleRef &mscorlibRef)
{
    // get interfaces
    CComPtr<IMetaDataEmit2> metaDataEmit;
    COM_FAIL_RETURN(m_profilerInfo3->GetModuleMetaData(moduleId, 
        ofRead | ofWrite, IID_IMetaDataEmit2, (IUnknown**)&metaDataEmit), S_OK);      
    
    CComPtr<IMetaDataAssemblyEmit> metaDataAssemblyEmit;
    COM_FAIL_RETURN(metaDataEmit->QueryInterface(
        IID_IMetaDataAssemblyEmit, (void**)&metaDataAssemblyEmit), S_OK);

    // find injected
    ASSEMBLYMETADATA assembly;
    ZeroMemory(&assembly, sizeof(assembly));
    assembly.usMajorVersion = 1;
    assembly.usMinorVersion = 0;
    assembly.usBuildNumber = 0; 
    assembly.usRevisionNumber = 0;
    COM_FAIL_RETURN(metaDataAssemblyEmit->DefineAssemblyRef(NULL, 
        0, L"Injected", &assembly, NULL, 0, 0, 
        &mscorlibRef), S_OK);
}

/// <summary>Handle <c>ICorProfilerCallback::ModuleAttachedToAssembly</c></summary>
/// <remarks>Inform the host that we have a new module attached and that it may be 
/// of interest</remarks>
HRESULT STDMETHODCALLTYPE CCodeInjection::ModuleAttachedToAssembly( 
    /* [in] */ ModuleID moduleId,
    /* [in] */ AssemblyID assemblyId)
{
    ULONG dwNameSize = 512;
    WCHAR szAssemblyName[512] = {0};
    COM_FAIL_RETURN(m_profilerInfo3->GetAssemblyInfo(assemblyId, 
        dwNameSize, &dwNameSize, szAssemblyName, NULL, NULL), S_OK);
    ATLTRACE(_T("::ModuleAttachedToAssembly(%X => ?, %X => %s)"), 
        moduleId, assemblyId, W2CT(szAssemblyName));

    if (lstrcmp(L"ProfilerTarget", szAssemblyName) == 0) {
        m_targetMethodRef = 0;
        // get reference to injected
        mdModuleRef injectedRef;
        COM_FAIL_RETURN(GetInjectedRef(moduleId, injectedRef), S_OK);

        // get interfaces
        CComPtr<IMetaDataEmit> metaDataEmit;
        COM_FAIL_RETURN(m_profilerInfo3->GetModuleMetaData(moduleId, 
            ofRead | ofWrite, IID_IMetaDataEmit, (IUnknown**)&metaDataEmit), S_OK);

        static COR_SIGNATURE methodCallSignature[] = 
        {
            IMAGE_CEE_CS_CALLCONV_DEFAULT,   
            0x01,                                   
            ELEMENT_TYPE_VOID,
            ELEMENT_TYPE_I4
        };

        // get base type and constructor
        mdTypeRef classTypeRef;
        COM_FAIL_RETURN(metaDataEmit->DefineTypeRefByName(injectedRef, 
             L"Injected.InjectedClass", &classTypeRef), S_OK);
        COM_FAIL_RETURN(metaDataEmit->DefineMemberRef(classTypeRef, 
            L"InjectedMethod", methodCallSignature, sizeof(methodCallSignature), 
            &m_targetMethodRef), S_OK);
    }

    return S_OK;
}

std::wstring CCodeInjection::GetMethodName(FunctionID functionId, 
    ModuleID& funcModule, mdToken& funcToken)
{
    ClassID funcClass;
	COM_FAIL_RETURN(m_profilerInfo3->GetFunctionInfo2(functionId, 
        NULL, &funcClass, &funcModule, &funcToken, 0, NULL, 
        NULL), std::wstring());

    CComPtr<IMetaDataImport2> metaDataImport2;
	COM_FAIL_RETURN(m_profilerInfo3->GetModuleMetaData(funcModule, 
        ofRead, IID_IMetaDataImport2, (IUnknown**) &metaDataImport2), std::wstring());

    ULONG dwNameSize = 512;
    WCHAR szMethodName[512] = {};
	COM_FAIL_RETURN(metaDataImport2->GetMethodProps(funcToken, NULL, 
        szMethodName, dwNameSize, &dwNameSize, NULL, 
        NULL, NULL, NULL, NULL), S_OK);

   	mdTypeDef typeDef;
	COM_FAIL_RETURN(m_profilerInfo3->GetClassIDInfo(funcClass, 
        NULL, &typeDef), std::wstring());

    dwNameSize = 512;
    WCHAR szClassName[512] = {};
    DWORD typeDefFlags = 0;

    COM_FAIL_RETURN(metaDataImport2->GetTypeDefProps(typeDef, szClassName, 
        dwNameSize, &dwNameSize, &typeDefFlags, NULL), std::wstring());

    std::wstring name = szClassName;
    name += L".";
    name += szMethodName;
    return name;
}

/// <summary>Handle <c>ICorProfilerCallback::JITCompilationStarted</c></summary>
/// <remarks>The 'workhorse' </remarks>
HRESULT STDMETHODCALLTYPE CCodeInjection::JITCompilationStarted( 
        /* [in] */ FunctionID functionId, /* [in] */ BOOL fIsSafeToBlock) 
{
    ModuleID moduleId; mdToken funcToken;
    std::wstring methodName = GetMethodName(functionId, 
        moduleId, funcToken);
    ATLTRACE(_T("::JITCompilationStarted(%X -> %s)"), 
        functionId, W2CT(methodName.c_str()));

    if (L"ProfilerTarget.Program.TargetMethod" == methodName && 
        m_targetMethodRef !=0 ) {
        // get method body
        LPCBYTE pMethodHeader = NULL;
        ULONG iMethodSize = 0;
        COM_FAIL_RETURN(m_profilerInfo3->GetILFunctionBody(
            moduleId, funcToken, &pMethodHeader, &iMethodSize), 
            S_OK);

        // parse IL
        Method instMethod((IMAGE_COR_ILMETHOD*)pMethodHeader); // <--

        // insert new IL block
        InstructionList instructions;
        instructions.push_back(new Instruction(CEE_LDARG_0));
        instructions.push_back(new Instruction(CEE_CALL, m_targetMethodRef));

        instMethod.InsertSequenceInstructionsAtOriginalOffset(
            1, instructions);

        instMethod.DumpIL();

        // allocate memory
        CComPtr<IMethodMalloc> methodMalloc;
        COM_FAIL_RETURN(m_profilerInfo3->GetILFunctionBodyAllocator(
            moduleId, &methodMalloc), S_OK);
        void* pNewMethod = methodMalloc->Alloc(instMethod.GetMethodSize());

        // write new method
        instMethod.WriteMethod((IMAGE_COR_ILMETHOD*)pNewMethod);
        COM_FAIL_RETURN(m_profilerInfo3->SetILFunctionBody(moduleId, 
            funcToken, (LPCBYTE) pNewMethod), S_OK);
    }

    return S_OK;
}