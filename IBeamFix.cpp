/******************************************************************************
IBeamFix

Copyright (C) 2020 by Mukunda Johnson <mukunda@mukunda.com>
 
Permission to use, copy, modify, and/or distribute this software for any purpose
with or without fee is hereby granted.
 
THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
******************************************************************************/
#include <iostream>
#include <fstream>
#include <vector>
#include <codecvt>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
///////////////////////////////////////////////////////////////////////////////
// Of course we could find this automatically, but having someone type it in
//  offers flexibility of what to modify (what if they want to try it out first
//  and then overwrite it themselve), as well as a way to confirm that they
//                      really want to potentially screw with their user32.dll.
const char USAGE[] = "Usage: IBeamFix.exe <path to user32.dll>";
//-----------------------------------------------------------------------------
// This is the contents of the original cursor that we want to match. It won't
//                                         match if the patch was done already.
std::vector<std::uint8_t> signature_data;
//-----------------------------------------------------------------------------
// User defined type for the enumeration callback to capture the result.
struct EnumerationResult {
	bool found      = false;
	LPCWSTR lpName  = NULL;
	WORD wLanguage  = 0;
	std::wstring name;
};

//-----------------------------------------------------------------------------
// Returns true if the signature matches the source byte array given.
bool CheckSignature( std::uint8_t *source, int size ) {
	// int size? INT? Yes, I absolutely hate unsigned ints everywhere.
	if( size != signature_data.size() ) return false;
	for( int i = 0; i < size; i++ ) {
		if( source[i] != signature_data[i] ) return false;
	}
	return true;
}

//-----------------------------------------------------------------------------
// Callback for enumerating over the cursor keys in the dll.
BOOL CALLBACK MyCursorEnumerator( HMODULE hModule, LPCWSTR lpType,
	                                         LPWSTR lpName, LONG_PTR lParam ) {	
	if( HRSRC rs = FindResource( hModule, lpName, lpType ) ) {
		HGLOBAL loadedResource = LoadResource( hModule, rs );
		// Documents say that this doesn't actually lock a resource - it just
		//                 maps its data to memory and the name is confusing.
		LPVOID resourceAddress = LockResource( loadedResource );
		DWORD size = SizeofResource( hModule, rs );
		std::cout << resourceAddress << std::endl;

		auto *bytes = reinterpret_cast<unsigned char*>(resourceAddress);

		bool match = CheckSignature( bytes, size );
		if( match ) {
			auto &result = *reinterpret_cast<EnumerationResult*>(lParam);
			result.found = true;
			if( IS_INTRESOURCE( lpName )) {
				result.lpName = lpName;
			} else {
				result.name = lpName;
				result.lpName = result.name.c_str();
			}
			return FALSE; // FALSE means break from the scan.
		}
	}
	
	return TRUE; // TRUE means continue the scan.
}

//-----------------------------------------------------------------------------
BOOL CALLBACK MyLanguageEnumerator( HMODULE hModule, LPCWSTR lpType,
	                                LPCWSTR lpName, WORD wLanguage,
	                                LONG_PTR lParam ) {
	auto &result = *reinterpret_cast<EnumerationResult*>(lParam);
	result.wLanguage = wLanguage;
	return FALSE;
}

//-----------------------------------------------------------------------------
// It's shit like this that makes you hate C++. Thank goodness I already wrote
//  this before.
//
// Converts a UTF-8 string to UTF-16. std::string -> std::wstring.
std::wstring UTF8toWstring( const std::string &input ) {
   
   // Passing 0 as output buffer size calculates the output buffer size.
   int computed_length = MultiByteToWideChar( CP_UTF8, 0,
                                          input.c_str(), -1, nullptr, 0 );
   if( computed_length <= 0 ) {
      return L"<Error>";
   }

   std::wstring output;

   // I don't think there's an easy way to resize and skip the
   //  initializaton.
   output.resize( computed_length );
   wchar_t *dataptr = output.data();

   int result = MultiByteToWideChar( CP_UTF8, 0,
                              input.c_str(), -1, dataptr, computed_length );

   if( result <= 0 ) {
      return L"<Error>";
   }

   return output;
}

//-----------------------------------------------------------------------------
// Loads binary data and returns it as a vector.
std::vector<uint8_t> LoadBlob( const std::string &filename ) {
	std::ifstream input( filename, std::ios::binary );
	std::vector<uint8_t> data;
	while( !input.eof() ) data.push_back( input.get() );
	data.pop_back(); // Last byte is invalid (attempt to read past the end).

	return std::move(data);
}

//-----------------------------------------------------------------------------
int main( int argc, char *argv[] ) {
	
	if( argc < 2 ) {
		std::cout << USAGE;
		return 0;
	}

	std::wstring dll_file_path = UTF8toWstring( argv[1] );

	// The signature file is a copy of the cursor resource that we are going
	//  to modify. If it doesn't exactly match, then we quit before touching
	//  anything.
	std::cout << "Loading signature..." << std::endl;
	signature_data = LoadBlob( "signature.bin" );
	
	// We need to find out the ID of the cursor in the user32.dll file.
	// Normally it's 73, but that might change in the future (or the past?).
	std::cout << "Loading DLL..." << std::endl;
	HMODULE lib = LoadLibraryEx( dll_file_path.c_str(), NULL,
		                                  LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE );
	if( !lib ) {
		std::cout << "Failed to load DLL." << std::endl;
		return 1;
	}

	// So we enumerate through the cursor keys, using the callback to check
	//  our signature for a match. It updates the userdata that we pass in.
	EnumerationResult result;
	EnumResourceNamesEx( lib, RT_CURSOR, MyCursorEnumerator,
		                           reinterpret_cast<LONG_PTR>(&result), 0, 0 );
	
	if( result.found ) {
		EnumResourceLanguages( lib, RT_CURSOR, result.lpName,
			       MyLanguageEnumerator, reinterpret_cast<LONG_PTR>(&result) );
		if( !result.wLanguage ) {
			std::cout << "Something went wrong. Couldn't find resource language."
				          << std::endl;
			return 7;
		}

		// Close this so we can make changes.
		FreeLibrary( lib );

		// We found a match in the DLL file, so we can try to update that.
		//  result.name is the name of the resource we want to update.
		HANDLE hUpdate;
		if( !(hUpdate = BeginUpdateResource( dll_file_path.c_str(), FALSE )) ) {
			std::cout << "Couldn't open DLL for updating. Error "
				         << GetLastError() << std::endl;
			return 2;
		}

		// Modifying our signature data to have a corrected hotspot.
		// First byte is the horizontal hotspot, change it from 8 to 10.
		if( signature_data[0] != 8 ) {
			// Make sure that we're doing something expected though.
			std::cout << "Unexpected signature." << std::endl;
			return 3;
		}
		signature_data[0] = 10;

		// WinAPI is so nasty. :)
		if( !UpdateResource( hUpdate, RT_CURSOR, result.lpName,
			               result.wLanguage,
			               reinterpret_cast<LPVOID>(signature_data.data()),
			                                         signature_data.size() )) {
			std::cout << "Couldn't update resource. Error "
				         << GetLastError() << std::endl;
			return 4;
		}

		if( !EndUpdateResource( hUpdate, FALSE )) {
			// Unsure if this failure can trigger and the update still goes through.
			std::cout << "Something went wrong. Couldn't end the resource update properly. Error "
				         << GetLastError() << std::endl;
			return 5;
		}

	} else {
		std::cout << "Couldn't find signature in DLL."
			         " Perhaps it was already patched?"
			         << std::endl;
		return 6;
	}
	
	std::cout << "Patched successfully." << std::endl;

	return 0;
}
