//
// $Id$
//

//
// Copyright (c) 2001-2010, Andrew Aksyonoff
// Copyright (c) 2008-2010, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxexcerpt.h"
#include "sphinxutils.h"
#include "sphinxsearch.h"
#include "sphinxquery.h"
#include "sphinxint.h"

#include <ctype.h>

/////////////////////////////////////////////////////////////////////////////
// THE EXCERPTS GENERATOR
/////////////////////////////////////////////////////////////////////////////

static const int MAX_HIGHLIGHT_WORDS = 256;

#define UINT32_MASK 0xffffffffUL
#define UINT16_MASK 0xffff
typedef uint64_t ZonePacked_t;

class ExcerptGen_c
{
	friend class SnippetsQwordSetup;

public:
							ExcerptGen_c ();
							~ExcerptGen_c () {}

	char *	BuildExcerpt ( const ExcerptQuery_t &, CSphDict * pDict, ISphTokenizer * pTokenizer );

	void	TokenizeQuery ( const ExcerptQuery_t &, CSphDict * pDict, ISphTokenizer * pTokenizer, const CSphIndexSettings & tSettings );
	void	TokenizeDocument ( char * pData, int iDataLen, CSphDict * pDict, ISphTokenizer * pTokenizer, bool bFillMasks, bool bRetainHtml, int iSPZ, const CSphIndexSettings & tSettings );

	void	SetMarker ( CSphHitMarker * pMarker ) { m_pMarker = pMarker; }

public:
	enum Token_e
	{
		TOK_NONE = 0,		///< unspecified type, also used as the end marker
		TOK_WORD,			///< just a word
		TOK_SPACE,			///< whitespace chars seq
		TOK_BREAK,			///< non-word chars seq which delimit a phrase part or boundary
		TOK_SPZ				///< SENTENCE, PARAGRAPH, ZONE
	};

	struct Token_t
	{
		Token_e				m_eType;		///< token type
		int					m_iStart;		///< token start (index in codepoints array)
		int					m_iLengthCP;	///< token length (in codepoints)
		int					m_iLengthBytes;	///< token length (in bytes)
		int					m_iWeight;		///< token weight
		DWORD				m_uWords;		///< matching query words mask
		SphWordID_t			m_iWordID;		///< token word ID from dictionary
		DWORD				m_uPosition;	///< hit position in document
	};

	struct Passage_t
	{
		int					m_iStart;			///< start token index
		int					m_iTokens;			///< token count
		int					m_iCodes;			///< codepoints count
		int					m_iWords;			///< words count
		DWORD				m_uQwords;			///< matching query words mask
		int					m_iQwordsWeight;	///< passage weight factor
		int					m_iQwordCount;		///< passage weight factor
		int			m_iMaxLCS;					///< passage weight factor
		int			m_iMinGap;					///< passage weight factor
		int			m_iStartLimit;				///< start of match in passage
		int			m_iEndLimit;				///< end of match in passage

		void Reset ()
		{
			m_iStart = 0;
			m_iTokens = 0;
			m_iCodes = 0;
			m_uQwords = 0;
			m_iQwordsWeight = 0;
			m_iQwordCount = 0;
			m_iMaxLCS = 0;
			m_iMinGap = 0;
		}

		inline int GetWeight () const
		{
			return m_iQwordCount + m_iQwordsWeight*m_iMaxLCS + m_iMinGap;
		}
	};

	enum KeywordStar_e
	{
		STAR_NONE	= 0,
		STAR_FRONT	= 1 << 0,
		STAR_BACK	= 1 << 1,
		STAR_BOTH	= STAR_FRONT | STAR_BACK
	};

	struct Keyword_t
	{
		int		m_uStar;
		int		m_iWord;
		int		m_iLength;
	};

	// ZonePacked_t bits:
	// 64-32	(32) - uint position ( in document words )
	// 32-16	(16) - uint sibling ( index of sibling (open/closed) zone instance )
	// 16-0		(0) - zone type for this instance
	const CSphVector<ZonePacked_t> & GetZones () const { return m_dZones; }
	const SmallStringHash_T<int> & GetZonesName () const { return m_hZones; }

protected:
	CSphVector<Token_t>		m_dTokens;		///< source text tokens
	CSphVector<Token_t>		m_dWords;		///< query words tokens
	int						m_iDocumentWords;
	int						m_iPassageId;

	CSphString				m_sBuffer; // FIXME!!! REMOVE!!! ME!!!

	CSphVector<BYTE>		m_dResult;		///< result holder
	int						m_iResultLen;	///< result codepoints count

	CSphVector<Passage_t>	m_dPassages;	///< extracted passages

	bool					m_bExactPhrase;

	DWORD					m_uFoundWords;	///< found words mask
	int						m_iQwordCount;
	int						m_iLastWord;
	CSphHitMarker *			m_pMarker;

	CSphVector<char>		m_dKeywordsBuffer;
	CSphVector<Keyword_t>	m_dKeywords;

	CSphVector<ZonePacked_t>	m_dZones;		///< zones for current document
	SmallStringHash_T<int>		m_hZones;	///< zones names
	CSphVector<int>				m_dZonePos;	///< zones positions (in characters)
	CSphVector<int>				m_dZoneParent;	///< zones parent type

protected:
	void					CalcPassageWeight ( const CSphVector<int> & dPassage, Passage_t & tPass, int iMaxWords, int iWordCountCoeff );
	bool					ExtractPassages ( const ExcerptQuery_t & q );
	bool					ExtractPhrases ( const ExcerptQuery_t & q );

	void					HighlightPhrase ( const ExcerptQuery_t & q, int iStart, int iEnd );
	void					HighlightAll ( const ExcerptQuery_t & q );
	void					HighlightStart ( const ExcerptQuery_t & q );
	bool					HighlightBestPassages ( const ExcerptQuery_t & q );

	void					ResultEmit ( const char * sLine, int iPassageId=0 );
	void					ResultEmit ( const Token_t & sTok );

	void					AddJunk ( int iStart, int iLength, int iBoundary );
	void					AddBoundary ();

	void					MarkHits ();

	bool					SetupWindow ( CSphVector<int> & dPass, Passage_t & tPass, int iFrom, int iCpLimit, int iMaxWords );
	void					FlushPassage ( const Passage_t & tPass, int iLCSThresh );
};

// find string sFind in first iLimit characters of sBuffer
static BYTE * FindString ( BYTE * sBuffer, BYTE * sFind, int iLimit )
{
	assert ( iLimit > 0 );
	assert ( sBuffer );
	assert ( sFind );

	iLimit++;
	do
	{
		while ( *sBuffer!=*sFind )
			if ( !*++sBuffer || !--iLimit ) return NULL;

		int iSubLimit = iLimit;
		BYTE * sSubFind = sFind;
		BYTE * sSubBuffer = sBuffer;
		while ( *sSubFind && *sSubBuffer && *sSubFind==*sSubBuffer++ )
		{
			sSubFind++;
			if ( !--iSubLimit ) return NULL;
		}
		if ( !*sSubFind )
			return sBuffer;
	}
	while ( *++sBuffer );

	return NULL;
}

/// hitman used here in snippets
typedef Hitman_c<8> HITMAN;

/// snippets query words for different cases
class ISnippetsQword: public ISphQword
{
public:
	CSphString *							m_sBuffer;
	CSphVector<ExcerptGen_c::Token_t> *		m_dTokens;
	ISphTokenizer *							m_pTokenizer;

	// word information, filled during query word setup
	int			m_iWordLength;
	int			m_iLastIndex;
	DWORD		m_uWordMask;

	// iterator state
	CSphMatch	m_tMatch;
	int			m_iToken;
	int			m_iChunk;

	typedef ExcerptGen_c::Token_t Token_t;

	ISnippetsQword()
		: m_iToken ( 0 )
		, m_iChunk ( 0 )
	{}

	virtual void SeekHitlist ( SphOffset_t ) {}

	virtual const CSphMatch & GetNextDoc ( DWORD * )
	{
		m_uFields = 0xFFFFFFFFUL;
		if ( ( m_iChunk++ )==0 )
		{
			if ( GetNextHit()!=EMPTY_HIT )
			{
				m_tMatch.m_iDocID = 1;
				m_iToken--;
			} else
				m_tMatch.m_iDocID = 0;
		} else
			m_tMatch.m_iDocID = 0;
		return m_tMatch;
	}
};

/// exact match

struct SnippetsQword_Exact_c: public ISnippetsQword
{
	virtual Hitpos_t GetNextHit ()
	{
		while ( m_iToken < m_dTokens->GetLength() )
		{
			Token_t & tToken = (*m_dTokens)[m_iToken++];
			if ( tToken.m_eType!=ExcerptGen_c::TOK_WORD )
				continue;

			if ( tToken.m_iWordID==m_iWordID )
			{
				tToken.m_uWords |= m_uWordMask;
				return HITMAN::Create ( 0, tToken.m_uPosition, ( m_iToken-1 )==m_iLastIndex );
			}
		}
		return EMPTY_HIT;
	}
};

/// partial matches
template < typename COMPARE > struct SnippetsQword_c: public ISnippetsQword
{
	virtual Hitpos_t GetNextHit ()
	{
		while ( m_iToken < m_dTokens->GetLength() )
		{
			Token_t & tToken = (*m_dTokens)[m_iToken++];
			if ( tToken.m_eType!=ExcerptGen_c::TOK_WORD )
				continue;

			m_pTokenizer->SetBuffer ( (BYTE *) &m_sBuffer->cstr() [ tToken.m_iStart ], tToken.m_iLengthBytes );
			BYTE * sToken = m_pTokenizer->GetToken(); // OPTIMIZE? token can be memoized and shared between qwords
			if ( (*(COMPARE *)this).Match ( tToken, sToken ) )
			{
				tToken.m_uWords |= m_uWordMask;
				return HITMAN::Create ( 0, tToken.m_uPosition, ( m_iToken-1 )==m_iLastIndex );
			}
		}
		return EMPTY_HIT;
	}
};

struct SnippetsQword_StarFront_c: public SnippetsQword_c<SnippetsQword_StarFront_c>
{
	inline bool Match ( const Token_t & tToken, BYTE * sToken )
	{
		int iOffset = tToken.m_iLengthBytes - m_iWordLength;
		return iOffset>=0 &&
			memcmp ( m_sDictWord.cstr(), sToken + iOffset, m_iWordLength )==0;
	}
};

struct SnippetsQword_StarBack_c: public SnippetsQword_c<SnippetsQword_StarBack_c>
{
	inline bool Match ( const Token_t & tToken, BYTE * sToken )
	{
		return ( tToken.m_iLengthBytes>=m_iWordLength ) &&
			memcmp ( m_sDictWord.cstr(), sToken, m_iWordLength )==0;
	}
};

struct SnippetsQword_StarBoth_c: public SnippetsQword_c<SnippetsQword_StarBoth_c>
{
	inline bool Match ( const Token_t & tToken, BYTE * sToken )
	{
		return FindString ( sToken, (BYTE *)m_sDictWord.cstr(), tToken.m_iLengthBytes )!=NULL;
	}
};

/// snippets query word setup
class SnippetsQwordSetup: public ISphQwordSetup
{
	ExcerptGen_c *		m_pGenerator;
	ISphTokenizer *		m_pTokenizer;

public:
	SnippetsQwordSetup ( ExcerptGen_c * pGenerator, ISphTokenizer * pTokenizer )
		: m_pGenerator ( pGenerator )
		, m_pTokenizer ( pTokenizer )
	{}

	virtual ISphQword *		QwordSpawn ( const XQKeyword_t & tWord ) const;
	virtual bool			QwordSetup ( ISphQword * pQword ) const;
};

ISphQword * SnippetsQwordSetup::QwordSpawn ( const XQKeyword_t & tWord ) const
{
	switch ( tWord.m_uStarPosition )
	{
		case STAR_NONE:		return new SnippetsQword_Exact_c;
		case STAR_FRONT:	return new SnippetsQword_StarFront_c;
		case STAR_BACK:		return new SnippetsQword_StarBack_c;
		case STAR_BOTH:		return new SnippetsQword_StarBoth_c;
		default:			assert ( "impossible star position" && 0 ); return NULL;
	}
}

bool SnippetsQwordSetup::QwordSetup ( ISphQword * pQword ) const
{
	ISnippetsQword * pWord = dynamic_cast<ISnippetsQword *> ( pQword );
	if ( !pWord )
		assert ( "query word setup failed" && 0 );

	pWord->m_iLastIndex = m_pGenerator->m_iLastWord;
	pWord->m_uWordMask = 1 << (m_pGenerator->m_iQwordCount++);
	pWord->m_iWordLength = strlen ( pWord->m_sDictWord.cstr() );
	pWord->m_dTokens = &(m_pGenerator->m_dTokens);
	pWord->m_sBuffer = &(m_pGenerator->m_sBuffer);
	pWord->m_pTokenizer = m_pTokenizer;

	pWord->m_iDocs = 1;
	pWord->m_iHits = 1;
	pWord->m_bHasHitlist = true;

	// add dummy word, used for passage weighting
	const char * sWord = pWord->m_sDictWord.cstr();
	const int iLength = m_pTokenizer->IsUtf8() ? sphUTF8Len ( sWord ) : strlen ( sWord );
	m_pGenerator->m_dWords.Add().m_iLengthCP = iLength;

	return true;
}

/////////////////////////////////////////////////////////////////////////////

inline bool operator < ( const ExcerptGen_c::Token_t & a, const ExcerptGen_c::Token_t & b )
{
	if ( a.m_iLengthCP==b.m_iLengthCP )
		return a.m_iStart > b.m_iStart;
	return a.m_iLengthCP < b.m_iLengthCP;
}


inline bool operator < ( const ExcerptGen_c::Passage_t & a, const ExcerptGen_c::Passage_t & b )
{
	if ( a.GetWeight()==b.GetWeight() )
		return a.m_iCodes < b.m_iCodes;
	return a.GetWeight() < b.GetWeight();
}

ExcerptGen_c::ExcerptGen_c ()
{
	m_iQwordCount = 0;
	m_bExactPhrase = false;
	m_pMarker = NULL;
	m_uFoundWords = 0;
}

void ExcerptGen_c::AddBoundary()
{
	Token_t & tLast = m_dTokens.Add();
	tLast.m_eType = TOK_BREAK;
	tLast.m_iStart = 0;
	tLast.m_iLengthBytes = 0;
	tLast.m_iWordID = 0;
	tLast.m_uWords = 0;
	tLast.m_uPosition = 0;
}

void ExcerptGen_c::AddJunk ( int iStart, int iLength, int iBoundary )
{
	assert ( iLength>0 );
	assert ( iLength<=m_sBuffer.Length() );
	assert ( iStart+iLength<=m_sBuffer.Length() );

	int iChunkStart = iStart;
	int iSaved = 0;

	for ( int i = iStart; i < iStart+iLength; i++ )
		if ( sphIsSpace ( m_sBuffer.cstr () [i] )!=sphIsSpace ( m_sBuffer.cstr () [iChunkStart] ) )
		{
			Token_t & tLast = m_dTokens.Add();
			tLast.m_eType = TOK_SPACE;
			tLast.m_iStart = iChunkStart;
			tLast.m_iLengthBytes = i - iChunkStart;
			tLast.m_iWordID = 0;
			tLast.m_uWords = 0;
			tLast.m_uPosition = 0;

			iChunkStart = i;
			iSaved += tLast.m_iLengthBytes;

			if ( iBoundary!=-1 && iSaved > ( iBoundary-iStart ) )
			{
				AddBoundary();
				iBoundary = -1;
			}
		}

	Token_t & tLast = m_dTokens.Add();
	tLast.m_eType = TOK_SPACE;
	tLast.m_iStart = iChunkStart;
	tLast.m_iLengthBytes = iStart + iLength - iChunkStart;
	tLast.m_iWordID = 0;
	tLast.m_uWords = 0;
	tLast.m_uPosition = 0;

	if ( iBoundary!=-1 )
		AddBoundary();
}


void ExcerptGen_c::TokenizeQuery ( const ExcerptQuery_t & tQuery, CSphDict * pDict, ISphTokenizer * pTokenizer, const CSphIndexSettings & tSettings )
{
	const bool bUtf8 = pTokenizer->IsUtf8();

	// tokenize query words
	int iWordsLength = strlen ( tQuery.m_sWords.cstr() );

	m_dKeywords.Reserve ( MAX_HIGHLIGHT_WORDS );

	BYTE * sWord;
	int iKwIndex = 0;
	int uPosition = 0;

	pTokenizer->SetBuffer ( (BYTE *)tQuery.m_sWords.cstr(), iWordsLength );
	while ( ( sWord = pTokenizer->GetToken() )!=NULL )
	{
		SphWordID_t iWord = pDict->GetWordID ( sWord );

		if ( pTokenizer->GetBoundary() )
			uPosition += tSettings.m_iBoundaryStep;

		bool bIsStopWord = false;
		if ( !iWord )
			bIsStopWord = pDict->IsStopWord ( sWord );

		if ( iWord || bIsStopWord )
			uPosition = bIsStopWord ? uPosition+tSettings.m_iStopwordStep : uPosition+1;

		if ( iWord )
		{
			Token_t & tLast = m_dWords.Add();
			tLast.m_eType = TOK_WORD;
			tLast.m_iWordID = iWord;
			tLast.m_iLengthBytes = strlen ( (const char *)sWord );
			tLast.m_iLengthCP = bUtf8 ? sphUTF8Len ( (const char *)sWord ) : tLast.m_iLengthBytes;
			tLast.m_uPosition = uPosition;

			// store keyword
			Keyword_t & kwLast = m_dKeywords.Add();
			kwLast.m_iLength = tLast.m_iLengthCP;

			// find stars
			bool bStarBack = ( *pTokenizer->GetTokenEnd()=='*' );
			bool bStarFront = ( pTokenizer->GetTokenStart()!=pTokenizer->GetBufferPtr() ) &&
				( pTokenizer->GetTokenStart()[-1]=='*' );
			kwLast.m_uStar = ( bStarFront ? STAR_FRONT : 0 ) | ( bStarBack ? STAR_BACK : 0 );

			// store token
			const int iEndIndex = iKwIndex + tLast.m_iLengthBytes + 1;
			m_dKeywordsBuffer.Resize ( iEndIndex );
			kwLast.m_iWord = iKwIndex;
			strcpy ( &m_dKeywordsBuffer [ iKwIndex ], (const char *)sWord ); // NOLINT
			iKwIndex = iEndIndex;

			if ( m_dWords.GetLength()==MAX_HIGHLIGHT_WORDS )
				break;
		}
	}
}

static int FindTagEnd ( const char * sData )
{
	assert ( *sData=='<' );
	const char * s = sData+1;

	// we just scan until EOLN or tag end
	while ( *s && *s!='>' )
	{
		// exit on duplicate
		if ( *s=='<' )
			return -1;

		if ( *s=='\'' || *s=='"' )
			s = (const char *)SkipQuoted ( (const BYTE *)s );
		else
			s++;
	}

	if ( !*s )
		return -1;

	return s-sData;
}

uint64_t sphPackZone ( DWORD uPosition, int iSiblingIndex, int iZoneType )
{
	assert ( iSiblingIndex>=0 && iSiblingIndex<UINT16_MASK );
	assert ( iZoneType>=0 && iZoneType<UINT16_MASK );

	return ( ( (uint64_t)uPosition<<32 )
		| ( ( iSiblingIndex & UINT16_MASK )<<16 )
		| ( iZoneType & UINT16_MASK ) );
}

int FindAddZone ( const CSphString & sZone, SmallStringHash_T<int> & hZones )
{
	int * pZoneIndex = hZones ( sZone );
	if ( pZoneIndex )
		return *pZoneIndex;

	int iZone = hZones.GetLength();
	hZones.Add ( iZone, sZone );
	return iZone;
}

void ExcerptGen_c::TokenizeDocument ( char * pData, int iDataLen, CSphDict * pDict, ISphTokenizer * pTokenizer, bool bFillMasks, bool bRetainHtml, int iSPZ, const CSphIndexSettings & tSettings )
{
	m_iDocumentWords = 0;
	m_dTokens.Reserve ( 1024 );
	m_sBuffer = pData;

	pTokenizer->SetBuffer ( (BYTE*)pData, iDataLen );

	const char * pStartPtr = pTokenizer->GetBufferPtr ();
	const char * pLastTokenEnd = pStartPtr;

	assert ( pStartPtr && pLastTokenEnd );

	if ( bRetainHtml )
		pTokenizer->AddSpecials ( "<" );

	CSphVector<int> dZoneStack;

	BYTE * sWord;
	DWORD uPosition = 0; // hit position in document
	while ( ( sWord = pTokenizer->GetToken() )!=NULL )
	{
		if ( pTokenizer->TokenIsBlended() )
			continue;

		const char * pTokenStart = pTokenizer->GetTokenStart ();

		if ( pTokenStart!=pStartPtr && pTokenStart>pLastTokenEnd )
			AddJunk ( pLastTokenEnd - pStartPtr,
				pTokenStart - pLastTokenEnd,
				pTokenizer->GetBoundary() ? pTokenizer->GetBoundaryOffset() : -1 );

		if ( bRetainHtml && *pTokenStart=='<' )
		{
			int iTagEnd = FindTagEnd ( pTokenStart );
			if ( iTagEnd!=-1 )
			{
				assert ( pTokenStart+iTagEnd<pTokenizer->GetBufferEnd() );
				AddJunk ( pTokenStart-pStartPtr, iTagEnd+1, pTokenizer->GetBoundary() ? pTokenizer->GetBoundaryOffset() : -1 );
				pTokenizer->SetBufferPtr ( pTokenStart+iTagEnd+1 );
				pLastTokenEnd = pTokenStart+iTagEnd+1; // fix it up to prevent adding last chunk on exit
				continue;
			}
		}

		// handle SPZ tokens GE then needed
		// add SENTENCE, PARAGRAPH, ZONE token, do junks and tokenizer and pLastTokenEnd fix up
		// FIXME!!! it heavily depends on such this attitude MAGIC_CODE_SENTENCE < MAGIC_CODE_PARAGRAPH < MAGIC_CODE_ZONE
		if ( iSPZ && *sWord>=iSPZ && *sWord<=MAGIC_CODE_ZONE )
		{
			// SPZ token has position and could be last token too
			++uPosition;

			if ( m_dTokens.GetLength()==0 || m_dTokens.Last().m_eType!=TOK_SPZ )
			{
				Token_t & tLast = m_dTokens.Add();
				tLast.m_eType = TOK_SPZ;
				tLast.m_iStart = 0;
				tLast.m_iLengthBytes = 0;
				tLast.m_iWordID = 0;
				tLast.m_uWords = 0;
				tLast.m_uPosition = 0;

				if ( *sWord==MAGIC_CODE_SENTENCE )
				{
					tLast.m_iStart = pTokenStart-pStartPtr;
					tLast.m_iLengthBytes = 1;
				}

				// SPZ token has position and could be last token too
				m_iLastWord = m_dTokens.GetLength();
				pLastTokenEnd = pTokenizer->GetTokenEnd(); // fix it up to prevent adding last chunk on exit
			}

			if ( *sWord==MAGIC_CODE_ZONE && iSPZ==MAGIC_CODE_ZONE )
			{
				const char * pEnd = pTokenizer->GetBufferPtr();
				const char * pTagStart = pEnd;
				while ( *pEnd!=MAGIC_CODE_ZONE )
					pEnd++;
				pEnd++; // skip zone token too
				pTokenizer->SetBufferPtr ( pEnd );
				pLastTokenEnd = pEnd; // fix it up to prevent adding last chunk on exit

				// span's management
				if ( *pTagStart!='/' )	// open zone
				{
					// zone stack management
					int iSelf = m_dZones.GetLength();
					dZoneStack.Add ( iSelf );

					// add zone itself
					CSphString sZone;
					sZone.SetBinary ( pTagStart, pEnd-pTagStart-1 );
					int iZone = FindAddZone ( sZone, m_hZones );
					m_dZones.Add ( sphPackZone ( uPosition, iSelf, iZone ) );

					// zone position in characters
					m_dZonePos.Add ( pTagStart-pStartPtr );

					// for open zone the parent is the zone itself
					m_dZoneParent.Add ( iZone );
				} else					// close zone
				{
#ifndef NDEBUG
					// lets check open - close tags match
					assert ( dZoneStack.GetLength() && dZoneStack.Last()<m_dZones.GetLength() );
					int iOpening = m_dZonePos [ dZoneStack.Last() ];
					assert ( iOpening<pEnd-pStartPtr && strncmp ( pStartPtr+iOpening, pTagStart+1, pEnd-pTagStart-2 )==0 );
#endif
					CSphString sZone;
					sZone.SetBinary ( pTagStart+1, pEnd-pTagStart-2 );
					int iZone = FindAddZone ( sZone, m_hZones );
					int iOpen = dZoneStack.Last();
					int iClose = m_dZones.GetLength();
					uint64_t uOpenPacked = m_dZones[ iOpen ];
					DWORD uOpenPos = ( ( uOpenPacked>>32 ) & UINT32_MASK );
					assert ( iZone==( uOpenPacked & UINT16_MASK ) ); // check for zone's types match;

					m_dZones[iOpen] = sphPackZone ( uOpenPos, iClose, iZone );
					m_dZones.Add ( sphPackZone ( uPosition, iOpen, iZone ) );

					// zone position in characters
					m_dZonePos.Add ( pTagStart-pStartPtr );

					// for close zone the parent is the previous zone on stack
					int iParentZone = dZoneStack.GetLength()>2 ? dZoneStack[dZoneStack.GetLength()-2] : 0;
					uint64_t uParentPacked = m_dZones.GetLength() && iParentZone<m_dZones.GetLength() ? m_dZones[iParentZone] : 0;
					m_dZoneParent.Add ( uParentPacked & UINT16_MASK );

					// pop up current zone from zone's stack
					dZoneStack.Resize ( dZoneStack.GetLength()-1 );
				}
			}

			continue;
		}

		SphWordID_t iWord = pDict->GetWordID ( sWord );

		pLastTokenEnd = pTokenizer->GetTokenEnd ();

		if ( pTokenizer->GetBoundary() )
			uPosition += tSettings.m_iBoundaryStep;

		bool bIsStopWord = false;
		if ( !iWord )
			bIsStopWord = pDict->IsStopWord ( sWord );

		if ( iWord || bIsStopWord )
			uPosition = bIsStopWord ? uPosition+tSettings.m_iStopwordStep : uPosition+1;

		Token_t & tLast = m_dTokens.Add();
		tLast.m_eType = iWord ? TOK_WORD : TOK_SPACE;
		tLast.m_uPosition = iWord || bIsStopWord ? uPosition : 0;
		tLast.m_iStart = pTokenStart - pStartPtr;
		tLast.m_iLengthBytes = pLastTokenEnd - pTokenStart;
		tLast.m_iWordID = iWord;
		tLast.m_uWords = 0;
		if ( iWord )
			m_iDocumentWords++;

		m_iLastWord = iWord ? m_dTokens.GetLength() - 1 : m_iLastWord;

		// fill word mask
		if ( bFillMasks && iWord )
		{
			bool bMatch = false;
			int iOffset;

			ARRAY_FOREACH ( nWord, m_dWords )
			{
				const char * sKeyword = &m_dKeywordsBuffer [ m_dKeywords[nWord].m_iWord ];
				const Token_t & tToken = m_dWords[nWord];

				switch ( m_dKeywords[nWord].m_uStar )
				{
				case STAR_NONE:
					bMatch = ( iWord==tToken.m_iWordID );
					break;

				case STAR_FRONT:
					iOffset = tLast.m_iLengthBytes - tToken.m_iLengthBytes;
					bMatch = ( iOffset>=0 ) &&
						( memcmp ( sKeyword, sWord + iOffset, tToken.m_iLengthBytes )==0 );
					break;

				case STAR_BACK:
					bMatch = ( tLast.m_iLengthBytes>=tToken.m_iLengthBytes ) &&
						( memcmp ( sKeyword, sWord, tToken.m_iLengthBytes )==0 );
					break;

				case STAR_BOTH:
					bMatch = strstr ( (const char *)sWord, sKeyword )!=NULL;
					break;
				}

				if ( bMatch )
				{
					tLast.m_uWords |= 1UL<<nWord;
					m_uFoundWords |= 1UL<<nWord;
				}
			}
		}
	}

	// last space if any
	if ( pLastTokenEnd!=pTokenizer->GetBufferEnd() )
	{
		int iOffset = pTokenizer->GetBoundary() ? pTokenizer->GetBoundaryOffset() : -1;
		AddJunk ( pLastTokenEnd - pStartPtr, pTokenizer->GetBufferEnd () - pLastTokenEnd, iOffset );
	}

	Token_t & tLast = m_dTokens.Add();
	tLast.m_eType = TOK_NONE;
	tLast.m_iStart = 0;
	tLast.m_iLengthCP = 0;
	tLast.m_iLengthBytes = 0;
	tLast.m_iWeight = 0;
	tLast.m_iWordID = 0;
	tLast.m_uWords = 0;
	tLast.m_uPosition = 0;
}

void ExcerptGen_c::MarkHits ()
{
	assert ( m_pMarker );

	// mark
	CSphVector<SphHitMark_t> dMarked;
	dMarked.Reserve ( m_dTokens.GetLength() );
	m_pMarker->Mark ( dMarked );

	// fix-up word masks
	int iMarked = dMarked.GetLength();
	int iTokens = m_dTokens.GetLength();
	int i = 0, k = 0;
	while ( i < iTokens )
	{
		// sync
		while ( k < iMarked && m_dTokens[i].m_uPosition > dMarked[k].m_uPosition )
			k++;

		if ( k==iMarked ) // no more marked hits, clear tail
		{
			for ( ; i < iTokens; i++ )
				m_dTokens[i].m_uWords = 0;
			break;
		}

		// clear false matches
		while ( dMarked[k].m_uPosition > m_dTokens[i].m_uPosition )
			m_dTokens[i++].m_uWords = 0;

		// skip tokens covered by hit's span
		assert ( dMarked[k].m_uPosition==m_dTokens[i].m_uPosition );
		assert ( dMarked[k].m_uSpan>=1 );
		while ( dMarked[k].m_uSpan-- )
		{
			i++;
			while ( i < iTokens && !m_dTokens[i].m_uPosition ) i++;
		}
	}
}

char * ExcerptGen_c::BuildExcerpt ( const ExcerptQuery_t & tQuery, CSphDict *, ISphTokenizer * pTokenizer )
{
	m_iPassageId = tQuery.m_iPassageId;

	if ( tQuery.m_bHighlightQuery )
		MarkHits();

	// sum token lengths
	const bool bUtf8 = pTokenizer->IsUtf8();
	int iSourceCodes = 0;
	ARRAY_FOREACH ( i, m_dTokens )
	{
		m_dTokens[i].m_iWeight = 0;

		if ( m_dTokens[i].m_iLengthBytes )
		{
			if ( bUtf8 )
			{
				int iLen = sphUTF8Len ( m_sBuffer.SubString ( m_dTokens[i].m_iStart, m_dTokens[i].m_iLengthBytes ).cstr() );
				m_dTokens[i].m_iLengthCP = iLen;
			} else
				m_dTokens[i].m_iLengthCP = m_dTokens[i].m_iLengthBytes;
			iSourceCodes += m_dTokens[i].m_iLengthCP;
		} else
			m_dTokens[i].m_iLengthCP = 0;
	}

	m_bExactPhrase = tQuery.m_bExactPhrase && ( m_dWords.GetLength()>1 );

	// assign word weights
	ARRAY_FOREACH ( i, m_dWords )
		m_dWords[i].m_iWeight = m_dWords[i].m_iLengthCP; // FIXME! should obtain freqs from dict

	// reset result
	m_dResult.Reserve ( 16384 );
	m_dResult.Resize ( 0 );
	m_iResultLen = 0;

	// do highlighting
	if ( ( tQuery.m_iLimit<=0 || tQuery.m_iLimit>iSourceCodes )
		&& ( tQuery.m_iLimitWords<=0 || tQuery.m_iLimitWords>m_iDocumentWords ) )
	{
		HighlightAll ( tQuery );

	} else
	{
		if ( !( ExtractPassages ( tQuery ) && HighlightBestPassages ( tQuery ) ) )
			if ( !tQuery.m_bAllowEmpty )
				HighlightStart ( tQuery );
	}

	// alloc, fill and return the result
	m_dResult.Add ( 0 );
	char * pRes = new char [ m_dResult.GetLength() ];
	memcpy ( pRes, &m_dResult[0], m_dResult.GetLength() );
	m_dResult.Reset ();

	return pRes;
}


void ExcerptGen_c::HighlightPhrase ( const ExcerptQuery_t & q, int iTok, int iEnd )
{
	while ( iTok<=iEnd )
	{
		while ( iTok<=iEnd && !m_dTokens[iTok].m_uWords )
			ResultEmit ( m_dTokens[iTok++] );

		if ( iTok>iEnd )
			break;

		bool bMatch = true;
		int iWord = 0;
		int iStart = iTok;
		int iLastDocPos = -1;
		while ( iWord<m_dWords.GetLength() )
		{
			if ( ( iTok > iEnd ) ||
				!( m_dTokens[iTok].m_eType!=TOK_WORD || m_dTokens[iTok].m_uWords==( 1UL<<iWord++ ) ) )
			{
				bMatch = false;
				break;
			}

			if ( m_dTokens[iTok].m_eType==TOK_WORD )
				iLastDocPos = iTok;

			iTok++;

			if ( iLastDocPos!=-1
				&& m_dTokens[iTok].m_eType==TOK_WORD
				&& iTok<=iEnd && iWord<m_dWords.GetLength()
				&& ( m_dWords[iWord].m_uPosition-m_dWords[iWord-1].m_uPosition )!=( m_dTokens[iTok].m_uPosition-m_dTokens[iLastDocPos].m_uPosition ) )
			{
				bMatch = false;
				break;
			}
		}

		if ( !bMatch )
		{
			ResultEmit ( m_dTokens[iStart] );
			iTok = iStart + 1;
			continue;
		}

		ResultEmit ( q.m_sBeforeMatch.cstr(), m_iPassageId );
		while ( iStart<iTok )
			ResultEmit ( m_dTokens [ iStart++ ] );
		ResultEmit ( q.m_sAfterMatch.cstr(), m_iPassageId++ );
	}
}


void ExcerptGen_c::HighlightAll ( const ExcerptQuery_t & q )
{
	bool bOpen = false;
	const int iMaxTok = m_dTokens.GetLength()-1; // skip last one, it's TOK_NONE

	if ( m_bExactPhrase )
		HighlightPhrase ( q, 0, iMaxTok-1 );
	else
	{
		// bag of words
		for ( int iTok=0; iTok<iMaxTok; iTok++ )
		{
			if ( ( m_dTokens[iTok].m_uWords!=0 ) ^ bOpen )
			{
				if ( bOpen )
					ResultEmit ( q.m_sAfterMatch.cstr(), m_iPassageId++ );
				else
					ResultEmit ( q.m_sBeforeMatch.cstr(), m_iPassageId );
				bOpen = !bOpen;
			}
			ResultEmit ( m_dTokens[iTok] );
		}
		if ( bOpen )
			ResultEmit ( q.m_sAfterMatch.cstr(), m_iPassageId++ );
	}
}


void ExcerptGen_c::HighlightStart ( const ExcerptQuery_t & q )
{
	// no matches found. just show the starting tokens
	int i = 0;
	while ( m_iResultLen+m_dTokens[i].m_iLengthCP < q.m_iLimit )
	{
		ResultEmit ( m_dTokens[i++] );
		if ( i>=m_dTokens.GetLength() )
			break;
	}
	ResultEmit ( q.m_sChunkSeparator.cstr() );
}


void ExcerptGen_c::ResultEmit ( const char * sLine, int iPassageId )
{
	if ( !iPassageId )
	{
		// plain old emit
		while ( *sLine )
		{
			assert ( (*(BYTE*)sLine)<128 );
			m_dResult.Add ( *sLine++ );
			m_iResultLen++;
		}

	} else
	{
		// emit with (our only) macro expansion
		while ( *sLine )
		{
			if ( *sLine=='%' && strncmp ( sLine, "%PASSAGE_ID%", 12 )==0 )
			{
				char sBuf[16];
				snprintf ( sBuf, sizeof(sBuf), "%d", iPassageId );
				for ( const char * s = sBuf; *s; s++ )
				{
					m_dResult.Add ( *s );
					m_iResultLen++;
				}
				sLine += 12; // skip zee macro
			} else
			{
				assert ( (*(BYTE*)sLine)<128 );
				m_dResult.Add ( *sLine++ );
				m_iResultLen++;
			}
		}
	}
}


void ExcerptGen_c::ResultEmit ( const Token_t & sTok )
{
	for ( int i=0; i<sTok.m_iLengthBytes; i++ )
		m_dResult.Add ( m_sBuffer.cstr () [ i+sTok.m_iStart ] );

	m_iResultLen += sTok.m_iLengthCP;
}

/////////////////////////////////////////////////////////////////////////////

void ExcerptGen_c::CalcPassageWeight ( const CSphVector<int> & dPassage, Passage_t & tPass, int iMaxWords, int iWordCountCoeff )
{
	DWORD uLast = 0;
	int iLCS = 1;
	tPass.m_iMaxLCS = 1;

	// calc everything
	tPass.m_uQwords = 0;
	tPass.m_iMinGap = iMaxWords-1;
	tPass.m_iStartLimit = INT_MAX;
	tPass.m_iEndLimit = INT_MIN;

	ARRAY_FOREACH ( i, dPassage )
	{
		Token_t & tTok = m_dTokens[dPassage[i]];
		assert ( tTok.m_eType==TOK_WORD );

		// update mask
		tPass.m_uQwords |= tTok.m_uWords;

		// update match boundary
		if ( tTok.m_uWords )
		{
			tPass.m_iStartLimit = Min ( tPass.m_iStartLimit, dPassage[i] );
			tPass.m_iEndLimit = Max ( tPass.m_iEndLimit, dPassage[i] );
		}

		// update LCS
		uLast = tTok.m_uWords & ( uLast<<1 );
		if ( uLast )
		{
			iLCS++;
			tPass.m_iMaxLCS = Max ( iLCS, tPass.m_iMaxLCS );
		} else
		{
			iLCS = 1;
			uLast = tTok.m_uWords;
		}

		// update min gap
		if ( tTok.m_uWords )
		{
			tPass.m_iMinGap = Min ( tPass.m_iMinGap, i );
			tPass.m_iMinGap = Min ( tPass.m_iMinGap, dPassage.GetLength()-1-i );
		}
	}
	assert ( tPass.m_iMinGap>=0 );

	// calc final weight
	tPass.m_iQwordsWeight = 0;
	tPass.m_iQwordCount = 0;

	DWORD uWords = tPass.m_uQwords;
	for ( int iWord=0; uWords; uWords >>= 1, iWord++ )
		if ( uWords & 1 )
	{
		tPass.m_iQwordsWeight += m_dWords[iWord].m_iWeight;
		tPass.m_iQwordCount++;
	}

	tPass.m_iMaxLCS *= iMaxWords;
	tPass.m_iQwordCount *= iWordCountCoeff;
}

bool ExcerptGen_c::SetupWindow ( CSphVector<int> & dPass, Passage_t & tPass, int i, int iCpLimit, int iMaxWords )
{
	assert ( i>=0 && i<m_dTokens.GetLength() );

	tPass.Reset();
	tPass.m_iStart = i;

	for ( ; i<m_dTokens.GetLength(); i++ )
	{
		const Token_t & tToken = m_dTokens[i];

		// skip starting whitespace
		if ( tPass.m_iTokens==0 && tToken.m_eType!=TOK_WORD )
		{
			tPass.m_iStart++;
			continue;
		}

		// stop when the window is large enough or meet SPZ
		if ( ( tPass.m_iCodes + tToken.m_iLengthCP > iCpLimit ) || dPass.GetLength()==iMaxWords || tToken.m_eType==TOK_SPZ )
		{
			tPass.m_iTokens += ( tToken.m_eType==TOK_SPZ && tToken.m_iLengthBytes>0 ); // only MAGIC_CODE_SENTENCE has length
			return ( tToken.m_eType==TOK_SPZ );
		}

		// got token, update passage
		tPass.m_iTokens++;
		tPass.m_iCodes += tToken.m_iLengthCP;

		if ( tToken.m_eType==TOK_WORD )
			dPass.Add(i);
	}

	return false;
}

void ExcerptGen_c::FlushPassage ( const Passage_t & tPass, int iLCSThresh )
{
	if ( tPass.m_uQwords && tPass.m_iMaxLCS>=iLCSThresh )
	{
		// if it's the very first one, do add
		if ( !m_dPassages.GetLength() )
		{
			m_dPassages.Add ( tPass );
		} else
		{
			// check if it's new or better
			Passage_t & tLast = m_dPassages.Last();
			if ( tLast.m_uQwords!=tPass.m_uQwords || tLast.m_iStart + tLast.m_iTokens - 1 < tPass.m_iStart )
				m_dPassages.Add ( tPass );
			else if ( tLast.GetWeight() < tPass.GetWeight() )
				tLast = tPass; // better
		}
	}
}

bool ExcerptGen_c::ExtractPassages ( const ExcerptQuery_t & q )
{
	m_dPassages.Reserve ( 256 );
	m_dPassages.Resize ( 0 );

	if ( q.m_bUseBoundaries )
		return ExtractPhrases ( q );

	// my current passage
	CSphVector<int> dPass;
	Passage_t tPass;
	tPass.Reset ();

	int iMaxWords = 2*q.m_iAround+1;
	if ( q.m_iLimitWords )
		iMaxWords = Min ( iMaxWords, q.m_iLimitWords );
	int iLCSThresh = m_bExactPhrase ? m_dWords.GetLength()*iMaxWords : 0;
	int iCpLimit = q.m_iLimit ? q.m_iLimit : INT_MAX;

	// setup initial window
	bool bSPZ = SetupWindow ( dPass, tPass, 0, iCpLimit, iMaxWords );

	// move our window until the end of document
	const int iCount = m_dTokens.GetLength();
	for ( ;; )
	{
		// re-weight current passage, and check if it matches
		CalcPassageWeight ( dPass, tPass, iMaxWords, 0 );
		tPass.m_iWords = dPass.GetLength();

		FlushPassage ( tPass, iLCSThresh );

		int iToken = tPass.m_iStart + tPass.m_iTokens;
		assert ( iToken<=iCount );
		if ( iToken==iCount )
			break;

		if ( bSPZ )
		{
			dPass.Resize ( 0 );
			bSPZ = SetupWindow ( dPass, tPass, iToken, iCpLimit, iMaxWords );
			continue;
		}

		// add another word
		for ( ; iToken < iCount; iToken++ )
		{
			tPass.m_iTokens++;
			tPass.m_iCodes += m_dTokens[iToken].m_iLengthCP;
			if ( m_dTokens[iToken].m_eType==TOK_WORD )
			{
				dPass.Add ( iToken );
				break;
			}

			if ( m_dTokens[iToken].m_eType==TOK_SPZ )
			{
				bSPZ = true;
				break;
			}
		}
		if ( iToken==iCount || bSPZ )
				continue;

		// drop front tokens until the window fits into both word and CP limits
		while ( ( tPass.m_iCodes > iCpLimit || dPass.GetLength() > iMaxWords ) && tPass.m_iTokens!=1 )
		{
			if ( m_dTokens[tPass.m_iStart].m_eType==TOK_WORD )
				dPass.Remove ( 0 );

			tPass.m_iCodes -= m_dTokens[tPass.m_iStart].m_iLengthCP;
			tPass.m_iTokens--;
			tPass.m_iStart++;
		}
	}

	return m_dPassages.GetLength()!=0;
}


bool ExcerptGen_c::ExtractPhrases ( const ExcerptQuery_t & )
{
	int iMaxWords = 100;
	int iLCSThresh = m_bExactPhrase ? m_dWords.GetLength()*iMaxWords : 0;

	int iStart = 0;
	DWORD uWords = 0;

	ARRAY_FOREACH ( iTok, m_dTokens )
	{
		// phrase boundary found, go flush
		if ( m_dTokens[iTok].m_eType==TOK_BREAK || m_dTokens[iTok].m_eType==TOK_NONE )
		{
			int iEnd = iTok - 1;

			// emit non-empty phrases with matching words as passages
			if ( iStart<iEnd && uWords!=0 )
			{
				Passage_t tPass;
				tPass.Reset ();

				tPass.m_iStart = iStart;
				tPass.m_iTokens = iEnd-iStart+1;

				CSphVector<int> dPass;
				for ( int i=iStart; i<=iEnd; i++ )
				{
					tPass.m_iCodes += m_dTokens[i].m_iLengthCP;
					if ( m_dTokens[i].m_eType==TOK_WORD )
						dPass.Add ( i );
				}

				CalcPassageWeight ( dPass, tPass, iMaxWords, 10000 );
				if ( tPass.m_iMaxLCS>=iLCSThresh )
				{
					tPass.m_iWords = dPass.GetLength();
					m_dPassages.Add ( tPass );
				}
			}

			if ( m_dTokens[iTok].m_eType==TOK_NONE )
				break;

			iStart = iTok + 1;
			uWords = 0;
		}

		// just an incoming token
		if ( m_dTokens[iTok].m_eType==TOK_WORD )
			uWords |= m_dTokens[iTok].m_uWords;
	}

	return m_dPassages.GetLength()!=0;
}


struct PassageOrder_fn
{
	inline bool IsLess ( const ExcerptGen_c::Passage_t & a, const ExcerptGen_c::Passage_t & b ) const
	{
		return a.m_iStart < b.m_iStart;
	}
};


bool ExcerptGen_c::HighlightBestPassages ( const ExcerptQuery_t & tQuery )
{
	assert ( m_dPassages.GetLength() );

	// needed for "slightly outta limit" check below
	int iKeywordsLength = 0;
	ARRAY_FOREACH ( i, m_dKeywords )
		iKeywordsLength += m_dKeywords[i].m_iLength;

	// our limits
	int iMaxPassages = tQuery.m_iLimitPassages
		? Min ( m_dPassages.GetLength(), tQuery.m_iLimitPassages )
		: m_dPassages.GetLength();
	int iMaxWords = tQuery.m_iLimitWords ? tQuery.m_iLimitWords : INT_MAX;
	int iMaxCp = tQuery.m_iLimit ? tQuery.m_iLimit : INT_MAX;

	// our best passages
	CSphVector<Passage_t> dShow;
	DWORD uWords = 0; // mask of words in dShow so far
	int iTotalCodes = 0;
	int iTotalWords = 0;

	CSphVector<int> dWeights ( m_dPassages.GetLength() );
	ARRAY_FOREACH ( i, m_dPassages )
		dWeights[i] = m_dPassages[i].m_iQwordsWeight;

	// collect enough best passages to show all keywords and max out the limits
	// don't care much if we're going over limits in this loop, it will be tightened below
	bool bAll = false;
	while ( dShow.GetLength() < iMaxPassages )
	{
		// get next best passage
		int iBest = -1;
		ARRAY_FOREACH ( i, m_dPassages )
		{
			if ( m_dPassages[i].m_iCodes && ( iBest==-1 || m_dPassages[iBest] < m_dPassages[i] ) )
				iBest = i;
		}
		if ( iBest<0 )
			break;
		Passage_t & tBest = m_dPassages[iBest];

		// does this passage fit the limits?
		bool bFits = ( iTotalCodes + tBest.m_iCodes<=iMaxCp ) && ( iTotalWords + tBest.m_iWords<=iMaxWords );

		// all words will be shown and we're outta limit
		if ( uWords==m_uFoundWords && !bFits )
		{
			// there might be just enough space to partially display this passage
			if ( ( iTotalCodes + iKeywordsLength )<=tQuery.m_iLimit )
				dShow.Add ( tBest );
			break;
		}

		// merge incoming passages into previous ones
		bool bMerged = false;
		ARRAY_FOREACH_COND ( i, dShow, !bMerged )
		{
			const Passage_t & tPass = dShow[i];
			bMerged = ( ( tBest.m_iStartLimit>=tPass.m_iStartLimit && tBest.m_iStartLimit<=tPass.m_iEndLimit )
				|| ( tBest.m_iEndLimit>=tPass.m_iStartLimit && tBest.m_iEndLimit<=tPass.m_iEndLimit )
				|| ( tPass.m_iStartLimit>=tBest.m_iStartLimit && tPass.m_iStartLimit<=tBest.m_iEndLimit )
				|| ( tPass.m_iEndLimit>=tBest.m_iStartLimit && tPass.m_iEndLimit<=tBest.m_iEndLimit ) );

			if ( !bMerged )
				continue;

			Passage_t tMerged = tPass;
			tMerged.m_iStart = Min ( tPass.m_iStart, tBest.m_iStart );
			tMerged.m_iTokens = Max ( tPass.m_iStart+tPass.m_iTokens-tMerged.m_iStart, tBest.m_iStart+tBest.m_iTokens-tMerged.m_iStart );
			tMerged.m_uQwords |= tBest.m_uQwords;
			if ( tMerged.m_iStart!=tPass.m_iStart || tMerged.m_iTokens!=tPass.m_iTokens )
			{
				tMerged.m_iWords = 0;
				tMerged.m_iCodes = 0;
				for ( int iTok = 0; iTok<tMerged.m_iTokens; iTok++ )
				{
					const Token_t & tTok = m_dTokens[tMerged.m_iStart+iTok];
					if ( tTok.m_eType!=TOK_WORD )
						continue;

					tMerged.m_iWords++;
					tMerged.m_iCodes += tTok.m_iLengthCP;
				}

				iTotalWords += tMerged.m_iWords-tPass.m_iWords;
				iTotalCodes += tMerged.m_iCodes-tPass.m_iCodes;
			}
			uWords |= tMerged.m_uQwords;
			dShow[i] = tMerged;
		}


		if ( !bMerged )
		{
			// save it, despite limits or whatever, we'll tighten everything in the loop below
			dShow.Add ( tBest );
			uWords |= tBest.m_uQwords;
			iTotalWords += tBest.m_iWords;
			iTotalCodes += tBest.m_iCodes;
		}
		tBest.m_iCodes = 0; // no longer needed here, abusing to mark displayed passages

		// we just managed to show all words? do one final re-weighting run
		if ( !bAll && uWords==m_uFoundWords )
		{
			bAll = true;
			ARRAY_FOREACH ( i, m_dPassages )
				m_dPassages[i].m_iQwordsWeight = dWeights[i];
		}

		// if we're already showing all words, re-weighting is not needed any more
		if ( bAll )
			continue;

		// re-weight passages, adjust for new mask of shown words
		ARRAY_FOREACH ( i, m_dPassages )
		{
			if ( !m_dPassages[i].m_iCodes )
				continue;
			DWORD uMask = tBest.m_uQwords;
			for ( int iWord=0; uMask; iWord++, uMask >>= 1 )
				if ( ( uMask & 1 ) && ( m_dPassages[i].m_uQwords & ( 1UL<<iWord ) ) )
					m_dPassages[i].m_iQwordsWeight -= m_dWords[iWord].m_iWeight;
			m_dPassages[i].m_uQwords &= ~uWords;
		}
	}

	// if all passages won't fit into the limit, try to trim them a bit
	//
	// this step is skipped when use_boundaries is enabled, because
	// each passage must be a separate sentence (delimited by
	// boundaries) and we don't want to split them
	if ( ( iTotalCodes > iMaxCp || iTotalWords > iMaxWords ) && !tQuery.m_bUseBoundaries )
	{
		const int iKeepAround = ( tQuery.m_iAround + 1 ) / 2;
		// find limits for trimming.
		//
		// we want to have at least iKeepAround words before the first and
		// after the last query word in passage.
		ARRAY_FOREACH ( i, dShow )
		{
			// find first and last matching words in passage
			int iFirst = -1, iLast = -1;
			const int iStart = dShow[i].m_iStart;
			const int iEnd = dShow[i].m_iStart + dShow[i].m_iTokens;
			int iQueryWords = 0;
			for ( int j = iStart; j!=iEnd; j++ )
			{
				if ( m_dTokens[j].m_uWords )
				{
					if ( iFirst==-1 )
						iFirst = j;
					iLast = j;
					iQueryWords++;
				}
			}
			dShow[i].m_iStartLimit = iFirst;
			dShow[i].m_iEndLimit = iLast;
			// sometimes there's enough context inside the passage so we can trim a bit more
			int iAround = iKeepAround;
			if ( iQueryWords>=3 )
			{
				int iPassageWords = 0;
				for ( int j = iFirst+1; j<iLast; j++ )
					if ( m_dTokens[j].m_eType==TOK_WORD && !m_dTokens[j].m_uWords )
						iPassageWords++;
				if ( iPassageWords>=iQueryWords )
					iAround = 1;
			}
			// find start limit
			int iCount = 0;
			for ( int j = iFirst; j!=iStart; j-- )
				if ( m_dTokens[j-1].m_eType==TOK_WORD && ++iCount>=iAround )
				{
					dShow[i].m_iStartLimit = j-1;
					break;
				}
			// find end limit
			iCount = 0;
			for ( int j = iLast+1; j!=iEnd; j++ )
				if ( m_dTokens[j].m_eType==TOK_WORD && ++iCount>=iAround )
				{
					dShow[i].m_iEndLimit = j;
					break;
				}
		}

		// trim passages
		bool bFirst = true;
		bool bDone = false;
		int iCodes = iTotalCodes;
		while ( !bDone )
		{
			// drop one token from each passage starting from the least relevant
			for ( int i=dShow.GetLength(); i > 0; i-- )
			{
				Passage_t & tPassage = dShow[i-1];
				int iFirst = tPassage.m_iStart;
				int iLast = tPassage.m_iStart + tPassage.m_iTokens - 1;
				if ( iFirst!=tPassage.m_iStartLimit && ( bFirst || iLast==tPassage.m_iEndLimit ) )
				{
					// drop first
					if ( ( tQuery.m_bForceAllWords && m_dTokens[tPassage.m_iStart].m_uWords==0 )
						|| !tQuery.m_bForceAllWords )
						tPassage.m_iStart++;
					tPassage.m_iTokens--;
					tPassage.m_iCodes -= m_dTokens[iFirst].m_iLengthCP;
					iTotalCodes -= m_dTokens[iFirst].m_iLengthCP;

				} else if ( iLast!=tPassage.m_iEndLimit )
				{
					// drop last
					if ( ( tQuery.m_bForceAllWords && m_dTokens[tPassage.m_iStart+tPassage.m_iTokens-1].m_uWords==0 )
						|| !tQuery.m_bForceAllWords )
						tPassage.m_iTokens--;
					tPassage.m_iCodes -= m_dTokens[iLast].m_iLengthCP;
					iTotalCodes -= m_dTokens[iLast].m_iLengthCP;
				}
				if ( iTotalCodes<=iMaxCp )
				{
					bDone = true;
					break;
				}
			}
			if ( iTotalCodes==iCodes )
				break; // couldn't reduce anything
			iCodes = iTotalCodes;
			bFirst = !bFirst;
		}
	}

	// if passages still don't fit start dropping least significant ones, limit is sacred.
	while ( ( iTotalCodes > iMaxCp || iTotalWords > iMaxWords ) && !tQuery.m_bForceAllWords )
	{
		iTotalCodes -= dShow.Last().m_iCodes;
		iTotalWords -= dShow.Last().m_iWords;
		dShow.RemoveFast ( dShow.GetLength()-1 );
	}

	if ( !dShow.GetLength() )
		return false;

	// sort passages in the document order
	if ( !tQuery.m_bWeightOrder )
		dShow.Sort ( PassageOrder_fn() );


	/// show
	int iLast = -1;
	bool bEmitZones = tQuery.m_bEmitZones && m_dZones.GetLength();
	ARRAY_FOREACH ( i, dShow )
	{
		int iTok = dShow[i].m_iStart;
		int iEnd = iTok + dShow[i].m_iTokens - 1;

		if ( iTok>1+iLast || tQuery.m_bWeightOrder )
		{
			ResultEmit ( tQuery.m_sChunkSeparator.cstr() );
			// find and emit most enclosing zone
			if ( bEmitZones )
			{
				int iHighlightStart = m_dTokens[iTok].m_iStart;
				int iZone = FindSpan ( m_dZonePos, iHighlightStart );
				if ( iZone!=-1 )
				{
					int iParent = m_dZoneParent[iZone];
					m_hZones.IterateStart();
					while ( m_hZones.IterateNext() )
					{
						if ( m_hZones.IterateGet()!=iParent )
							continue;

						ResultEmit ( "<" );
						ResultEmit ( m_hZones.IterateGetKey().cstr() );
						ResultEmit ( ">" );
						break;
					}
				}
			}
		}

		if ( m_bExactPhrase )
			HighlightPhrase ( tQuery, iTok, iEnd );
		else
		{
			while ( iTok<=iEnd )
			{
				if ( iTok>iLast || tQuery.m_bWeightOrder )
				{
					if ( m_dTokens[iTok].m_uWords )
					{
						ResultEmit ( tQuery.m_sBeforeMatch.cstr(), m_iPassageId );
						ResultEmit ( m_dTokens[iTok] );
						ResultEmit ( tQuery.m_sAfterMatch.cstr(), m_iPassageId++ );
					} else
						ResultEmit ( m_dTokens[iTok] );
				}
				iTok++;
			}
		}

		iLast = tQuery.m_bWeightOrder ? iEnd : Max ( iLast, iEnd );
	}
	if ( m_dTokens[iLast].m_eType!=TOK_NONE && m_dTokens[iLast+1].m_eType!=TOK_NONE )
		ResultEmit ( tQuery.m_sChunkSeparator.cstr() );

	return true;
}

//////////////////////////////////////////////////////////////////////////
// FAST PATH FOR FULL DOCUMENT HIGHLIGHTING
//////////////////////////////////////////////////////////////////////////

struct DocQueryZonePair_t
{
	int m_iDoc;
	int m_iQuery;
	bool operator<( const DocQueryZonePair_t & b ) const { return m_iDoc<b.m_iDoc; }
	bool operator>( const DocQueryZonePair_t & b ) const { return m_iDoc>b.m_iDoc; }
	bool operator==( const DocQueryZonePair_t & b ) const { return m_iDoc==b.m_iDoc; }
};

/// hit-in-zone check implementation for the matching engine
class SnippetZoneChecker_c : public ISphZoneCheck
{
private:
	struct ZoneHits_t
	{
		CSphVector<Hitpos_t> m_dOpen;
		CSphVector<Hitpos_t> m_dClose;
	};

	CSphVector<ZoneHits_t> m_dZones;

public:
	SnippetZoneChecker_c ( const CSphVector<ZonePacked_t> & dDocZones, const SmallStringHash_T<int> & hDocNames, const CSphVector<CSphString> & dQueryZones )
	{
		if ( !dQueryZones.GetLength() )
			return;

		CSphVector<DocQueryZonePair_t> dCheckedZones;
		ARRAY_FOREACH ( i, dQueryZones )
		{
			int * pZone = hDocNames ( dQueryZones[i] );
			if ( pZone )
			{
				DocQueryZonePair_t & tPair = dCheckedZones.Add ();
				tPair.m_iDoc = *pZone;
				tPair.m_iQuery = i;
			}
		}

		dCheckedZones.Sort();
		m_dZones.Resize ( dQueryZones.GetLength() );

		ARRAY_FOREACH ( i, dDocZones )
		{
			uint64_t uZonePacked = dDocZones[i];
			DWORD uPos = ( ( uZonePacked >>32 ) & UINT32_MASK );
			int iSibling = ( ( uZonePacked>>16 ) & UINT16_MASK );
			int iZone = ( uZonePacked & UINT16_MASK );
			assert ( iSibling>=0 && iSibling<dDocZones.GetLength() );
			assert ( iZone==( dDocZones[iSibling] & UINT16_MASK ) );

			// skip cases:
			// + close zone (tSpan.m_iSibling<i) - skipped
			// + open without close zone (tSpan.m_iSibling==i) - skipped
			// + open zone position > close zone position
			// + zone type not in query zones
			if ( iSibling<=i || uPos>=( ( dDocZones[iSibling]>>32 ) & UINT32_MASK ) )
				continue;

			DocQueryZonePair_t tRefZone;
			tRefZone.m_iDoc = iZone;
			const DocQueryZonePair_t * pPair = dCheckedZones.BinarySearch ( tRefZone );
			if ( !pPair )
				continue;

			uint64_t uClosePacked = dDocZones[iSibling];
			DWORD uClosePos = ( ( uClosePacked>>32 ) & UINT32_MASK );

			ZoneHits_t & tZone = m_dZones[pPair->m_iQuery];
			tZone.m_dOpen.Add ( uPos );
			tZone.m_dClose.Add ( uClosePos );
		}

#ifndef NDEBUG
		ARRAY_FOREACH ( i, m_dZones )
		{
			const ZoneHits_t & tZone = m_dZones[i];
			assert ( tZone.m_dOpen.GetLength()==tZone.m_dClose.GetLength() );
			const Hitpos_t * pHit = tZone.m_dOpen.Begin()+1;
			const Hitpos_t * pMax = tZone.m_dOpen.Begin()+tZone.m_dOpen.GetLength();
			for ( ; pHit<pMax; pHit++ )
				assert ( pHit[-1]<pHit[0] );
			pHit = tZone.m_dClose.Begin()+1;
			pMax = tZone.m_dClose.Begin()+tZone.m_dClose.GetLength();
			for ( ; pHit<pMax; pHit++ )
				assert ( pHit[-1]<pHit[0] );
		}
#endif
	}

	virtual bool IsInZone ( int iZone, const ExtHit_t * pHit )
	{
		Hitpos_t uPos = HITMAN::GetLCS ( pHit->m_uHitpos );
		int iOpen = FindSpan ( m_dZones[iZone].m_dOpen, uPos );
		return ( iOpen>=0 && uPos<=m_dZones[iZone].m_dClose[iOpen] );
	}
};

/////////////////////////////////////////////////////////////////////////////

ExcerptQuery_t::ExcerptQuery_t ()
	: m_sBeforeMatch ( "<b>" )
	, m_sAfterMatch ( "</b>" )
	, m_sChunkSeparator ( " ... " )
	, m_sStripMode ( "index" )
	, m_iLimit ( 256 )
	, m_iLimitWords ( 0 )
	, m_iLimitPassages ( 0 )
	, m_iAround ( 5 )
	, m_iPassageId ( 1 )
	, m_iPassageBoundary ( 0 )
	, m_bRemoveSpaces ( false )
	, m_bExactPhrase ( false )
	, m_bUseBoundaries ( false )
	, m_bWeightOrder ( false )
	, m_bHighlightQuery ( false )
	, m_bForceAllWords ( false )
	, m_bLoadFiles ( false )
	, m_bAllowEmpty ( false )
	, m_bEmitZones ( false )
{
}


/////////////////////////////////////////////////////////////////////////////

char * sphBuildExcerpt ( ExcerptQuery_t & tOptions, CSphDict * pDict, ISphTokenizer * pTokenizer, const CSphSchema * pSchema, CSphIndex *pIndex, CSphString & sError, const CSphHTMLStripper * pStripper )
{
	if ( tOptions.m_sStripMode=="retain"
		&& !( tOptions.m_iLimit==0 && tOptions.m_iLimitPassages==0 && tOptions.m_iLimitWords==0 ) )
	{
		sError = "html_strip_mode=retain requires that all limits are zero";
		return NULL;
	}

	if ( !tOptions.m_sWords.cstr()[0] )
		tOptions.m_bHighlightQuery = false;

	char * pData = const_cast<char*> ( tOptions.m_sSource.cstr() );
	CSphScopedPtr<BYTE> pBuffer ( NULL );
	int iDataLen = tOptions.m_sSource.Length();

	if ( tOptions.m_bLoadFiles )
	{
		CSphAutofile tFile;
		if ( tFile.Open ( tOptions.m_sSource.cstr(), SPH_O_READ, sError )<0 )
			return NULL;

		// will this ever trigger? time will tell; email me if it does!
		if ( tFile.GetSize()+1>=(SphOffset_t)INT_MAX )
		{
			sError.SetSprintf ( "%s too big for snippet (over 2 GB)", pData );
			return NULL;
		}

		int iFileSize = (int)tFile.GetSize();
		if ( iFileSize<=0 )
		{
			static char sEmpty[] = "";
			return sEmpty;
		}

		iDataLen = iFileSize+1;
		pBuffer = new BYTE [ iDataLen ];
		if ( !tFile.Read ( pBuffer.Ptr(), iFileSize, sError ) )
			return NULL;

		pBuffer.Ptr()[iFileSize] = 0;
		pData = (char*) pBuffer.Ptr();
	}

	// strip if we have to
	if ( pStripper )
		pStripper->Strip ( (BYTE*)pData );

	// FIXME!!! check on real data (~100 Mb) as stripper changes len
	iDataLen = strlen ( pData );

	if ( !tOptions.m_bHighlightQuery )
	{
		// legacy highlighting
		ExcerptGen_c tGenerator;
		tGenerator.TokenizeQuery ( tOptions, pDict, pTokenizer, pIndex->GetSettings() );
		tGenerator.TokenizeDocument ( pData, iDataLen, pDict, pTokenizer, true, tOptions.m_sStripMode=="retain", tOptions.m_iPassageBoundary, pIndex->GetSettings() );
		return tGenerator.BuildExcerpt ( tOptions, pDict, pTokenizer );
	}

	XQQuery_t tQuery;
	if ( !sphParseExtendedQuery ( tQuery, tOptions.m_sWords.cstr(), pTokenizer, pSchema, pDict ) )
	{
		sError = tQuery.m_sParseError;
		return NULL;
	}
	tQuery.m_pRoot->ClearFieldMask();

	ExcerptGen_c tGenerator;
	tGenerator.TokenizeDocument ( pData, iDataLen, pDict, pTokenizer, false, tOptions.m_sStripMode=="retain", tOptions.m_iPassageBoundary, pIndex->GetSettings() );


	CSphScopedPtr<SnippetZoneChecker_c> pZoneChecker ( new SnippetZoneChecker_c ( tGenerator.GetZones(), tGenerator.GetZonesName(), tQuery.m_dZones ) );
	SnippetsQwordSetup tSetup ( &tGenerator, pTokenizer );
	CSphString sWarning;

	tSetup.m_pDict = pDict;
	tSetup.m_pIndex = pIndex;
	tSetup.m_eDocinfo = SPH_DOCINFO_EXTERN;
	tSetup.m_pWarning = &sWarning;
	tSetup.m_pZoneChecker = pZoneChecker.Ptr();

	CSphScopedPtr<CSphHitMarker> pMarker ( CSphHitMarker::Create ( tQuery.m_pRoot, tSetup ) );
	if ( !pMarker.Ptr() )
	{
		sError = sWarning;
		return NULL;
	}

	tGenerator.SetMarker ( pMarker.Ptr() );
	return tGenerator.BuildExcerpt ( tOptions, pDict, pTokenizer );
}

//
// $Id$
//
