#include <cmath>
#include <algorithm>

#include "macroblock.hh"
#include "raster.hh"

using namespace std;

template <unsigned int size>
Raster::Block<size>::Block( const typename TwoD< Block >::Context & c,
			    TwoD< uint8_t > & raster_component )
  : contents_( raster_component, size * c.column, size * c.row ),
    context_( c ),
    predictors_( context_ )
{}

/* the rightmost Y-subblocks in a macroblock (other than the upper-right subblock) are special-cased */
template <>
void Raster::Block4::set_above_right_bottom_row_predictor( const typename Predictors::AboveRightBottomRowPredictor & replacement )
{
  predictors_.above_right_bottom_row_predictor.above_right_bottom_row.set( replacement.above_right_bottom_row );
  predictors_.above_right_bottom_row_predictor.above_bottom_right_pixel = replacement.above_bottom_right_pixel;
  predictors_.above_right_bottom_row_predictor.use_row = replacement.use_row;
}

Raster::Macroblock::Macroblock( const TwoD< Macroblock >::Context & c, Raster & raster )
  : Y( raster.Y_bigblocks_.at( c.column, c.row ) ),
    U( raster.U_bigblocks_.at( c.column, c.row ) ),
    V( raster.V_bigblocks_.at( c.column, c.row ) ),
    Y_sub( raster.Y_subblocks_, 4 * c.column, 4 * c.row ),
    U_sub( raster.U_subblocks_, 2 * c.column, 2 * c.row ),
    V_sub( raster.V_subblocks_, 2 * c.column, 2 * c.row )
{
  /* adjust "extra pixels" for rightmost Y subblocks in macroblock (other than the top one) */
  for ( unsigned int row = 1; row < 4; row++ ) {
    Y_sub.at( 3, row ).set_above_right_bottom_row_predictor( Y_sub.at( 3, 0 ).predictors().above_right_bottom_row_predictor );
  }
}

Raster::Raster( const unsigned int display_width, const unsigned int display_height )
  : display_width_( display_width ), display_height_( display_height )
{
  if ( (display_width_ % 2) or (display_height_ % 2) ) {
    throw Unsupported( "display dimensions must be even" );
  }
}

template <unsigned int size>
const typename Raster::Block<size>::Row & Raster::Block<size>::Predictors::row127( void )
{
  static TwoD< uint8_t > storage( size, 1, 127 );
  static const Row row( storage, 0, 0 );
  return row;
}

template <unsigned int size>
const typename Raster::Block<size>::Column & Raster::Block<size>::Predictors::col129( void )
{
  static TwoD< uint8_t > storage( 1, size, 129 );
  static const Column col( storage, 0, 0 );
  return col;
}

template <unsigned int size>
Raster::Block<size>::Predictors::Predictors( const typename TwoD< Block >::Context & context )
  : above_row( context.above.initialized()
	       ? context.above.get()->contents().row( size - 1 )
	       : row127() ),
    left_column( context.left.initialized()
		 ? context.left.get()->contents().column( size - 1 )
		 : col129() ),
    above_left( context.above_left.initialized()
		? context.above_left.get()->at( size - 1, size - 1 )
		: ( context.above.initialized()
		    ? col129().at( 0, 0 )
		    : row127().at( 0, 0 ) ) ),
    above_right_bottom_row_predictor( { context.above_right.initialized()
	  ? context.above_right.get()->contents().row( size - 1 )
	  : row127(),
	  context.above.initialized()
	  ? &context.above.get()->at( size - 1, size - 1 )
	  : &row127().at( 0, 0 ),
	  context.above_right.initialized() } )
{}

template <unsigned int size>
uint8_t Raster::Block<size>::Predictors::AboveRightBottomRowPredictor::above_right( const unsigned int column ) const
{
  return use_row ? above_right_bottom_row.at( column, 0 ) : *above_bottom_right_pixel;
}

template <unsigned int size>
uint8_t Raster::Block<size>::Predictors::above( const int8_t column ) const
{
  assert( column >= -1 and column < int8_t( size * 2 ) );
  if ( column == -1 ) return above_left;
  if ( 0 <= column and column < int( size ) ) return above_row.at( column, 0 );
  return above_right_bottom_row_predictor.above_right( column - size );
}

template <unsigned int size>
uint8_t Raster::Block<size>::Predictors::left( const int8_t row ) const
{
  assert( row >= -1 and row < int8_t( size ) );
  if ( row == -1 ) return above_left;
  return left_column.at( 0, row );
}

template <unsigned int size>
uint8_t Raster::Block<size>::Predictors::east( const int8_t num ) const
{
  assert( 0 <= num and num <= int8_t( size * 2 ) );
  if ( num <= 4 ) { return left( 3 - num ); }
  return above( num - 5 );
}

template <unsigned int size>
void Raster::Block<size>::true_motion_predict( void )
{
  contents_.forall_ij( [&] ( uint8_t & b, unsigned int column, unsigned int row )
		       { b = clamp255( predictors().left_column.at( 0, row )
				       + predictors().above_row.at( column, 0 )
				       - predictors().above_left ); } );
}

template <unsigned int size>
void Raster::Block<size>::horizontal_predict( void )
{
  for ( unsigned int row = 0; row < size; row++ ) {
    contents_.row( row ).fill( predictors().left_column.at( 0, row ) );
  }
}

template <unsigned int size>
void Raster::Block<size>::vertical_predict( void )
{
  for ( unsigned int column = 0; column < size; column++ ) {
    contents_.column( column ).fill( predictors().above_row.at( column, 0 ) );
  }
}

template <unsigned int size>
void Raster::Block<size>::dc_predict_simple( void )
{
  static_assert( size == 4 or size == 8 or size == 16, "invalid Block size" );
  static constexpr uint8_t log2size = size == 4 ? 2 : size == 8 ? 3 : size == 16 ? 4 : 0;

  contents_.fill( ((predictors().above_row.sum(int16_t())
		    + predictors().left_column.sum(int16_t())) + (1 << log2size))
		  >> (log2size+1) );
}

template <unsigned int size>
void Raster::Block<size>::dc_predict( void )
{
  if ( context_.above.initialized() and context_.left.initialized() ) {
    return dc_predict_simple();
  }

  uint8_t value = 128;
  static_assert( size == 4 or size == 8 or size == 16, "invalid Block size" );
  static constexpr uint8_t log2size = size == 4 ? 2 : size == 8 ? 3 : size == 16 ? 4 : 0;

  if ( context_.above.initialized() ) {
    value = (predictors().above_row.sum(int16_t()) + (1 << (log2size-1))) >> log2size;
  } else if ( context_.left.initialized() ) {
    value = (predictors().left_column.sum(int16_t()) + (1 << (log2size-1))) >> log2size;
  }

  contents_.fill( value );
}

template <>
template <>
void Raster::Block8::intra_predict( const mbmode uv_mode )
{
  /* Chroma prediction */

  switch ( uv_mode ) {
  case DC_PRED: dc_predict(); break;
  case V_PRED:  vertical_predict();  break;
  case H_PRED:  horizontal_predict();  break;
  case TM_PRED: true_motion_predict(); break;
  default: assert( false ); break; /* tree decoder for uv_mode can't produce this */
  }
}

template <>
template <>
void Raster::Block16::intra_predict( const mbmode uv_mode )
{
  /* Y prediction for whole macroblock */

  switch ( uv_mode ) {
  case DC_PRED: dc_predict(); break;
  case V_PRED:  vertical_predict();  break;
  case H_PRED:  horizontal_predict();  break;
  case TM_PRED: true_motion_predict(); break;
  default: assert( false ); break; /* need to predict and transform subblocks independently */
  }
}

uint8_t avg3( const uint8_t x, const uint8_t y, const uint8_t z )
{
  return (x + 2 * y + z + 2) >> 2;
}

uint8_t avg2( const uint8_t x, const uint8_t y )
{
  return (x + y + 1) >> 1;
}

template <>
void Raster::Block4::vertical_smoothed_predict( void )
{
  contents_.column( 0 ).fill( avg3( above( -1 ), above( 0 ), above( 1 ) ) );
  contents_.column( 1 ).fill( avg3( above( 0 ),  above( 1 ), above( 2 ) ) );
  contents_.column( 2 ).fill( avg3( above( 1 ),  above( 2 ), above( 3 ) ) );
  contents_.column( 3 ).fill( avg3( above( 2 ),  above( 3 ), above( 4 ) ) );
}

template <>
void Raster::Block4::horizontal_smoothed_predict( void )
{
  contents_.row( 0 ).fill( avg3( left( -1 ), left( 0 ), left( 1 ) ) );
  contents_.row( 1 ).fill( avg3( left( 0 ),  left( 1 ), left( 2 ) ) );
  contents_.row( 2 ).fill( avg3( left( 1 ),  left( 2 ), left( 3 ) ) );
  contents_.row( 3 ).fill( avg3( left( 2 ),  left( 3 ), left( 3 ) ) );
  /* last line is special because we can't use left( 4 ) yet */
}

template <>
void Raster::Block4::left_down_predict( void )
{
  at( 0, 0 ) =                                        avg3( above( 0 ), above( 1 ), above( 2 ) );
  at( 1, 0 ) = at( 0, 1 ) =                           avg3( above( 1 ), above( 2 ), above( 3 ) );
  at( 2, 0 ) = at( 1, 1 ) = at( 0, 2 ) =              avg3( above( 2 ), above( 3 ), above( 4 ) );
  at( 3, 0 ) = at( 2, 1 ) = at( 1, 2 ) = at( 0, 3 ) = avg3( above( 3 ), above( 4 ), above( 5 ) );
  at( 3, 1 ) = at( 2, 2 ) = at( 1, 3 ) =              avg3( above( 4 ), above( 5 ), above( 6 ) );
  at( 3, 2 ) = at( 2, 3 ) =                           avg3( above( 5 ), above( 6 ), above( 7 ) );
  at( 3, 3 ) =                                        avg3( above( 6 ), above( 7 ), above( 7 ) );
  /* last line is special because we don't use above( 8 ) */
}

template <>
void Raster::Block4::right_down_predict( void )
{
  at( 0, 3 ) =                                        avg3( east( 0 ), east( 1 ), east( 2 ) );
  at( 1, 3 ) = at( 0, 2 ) =                           avg3( east( 1 ), east( 2 ), east( 3 ) );
  at( 2, 3 ) = at( 1, 2 ) = at( 0, 1 ) =              avg3( east( 2 ), east( 3 ), east( 4 ) );
  at( 3, 3 ) = at( 2, 2 ) = at( 1, 1 ) = at( 0, 0 ) = avg3( east( 3 ), east( 4 ), east( 5 ) );
  at( 3, 2 ) = at( 2, 1 ) = at( 1, 0 ) =              avg3( east( 4 ), east( 5 ), east( 6 ) );
  at( 3, 1 ) = at( 2, 0 ) =                           avg3( east( 5 ), east( 6 ), east( 7 ) );
  at( 3, 0 ) =                                        avg3( east( 6 ), east( 7 ), east( 8 ) );
}

template <>
void Raster::Block4::vertical_right_predict( void )
{
  at( 0, 3 ) =                                        avg3( east( 1 ), east( 2 ), east( 3 ) );
  at( 0, 2 ) =                                        avg3( east( 2 ), east( 3 ), east( 4 ) );
  at( 1, 3 ) = at( 0, 1 ) =                           avg3( east( 3 ), east( 4 ), east( 5 ) );
  at( 1, 2 ) = at( 0, 0 ) =                           avg2( east( 4 ), east( 5 ) );
  at( 2, 3 ) = at( 1, 1 ) =                           avg3( east( 4 ), east( 5 ), east( 6 ) );
  at( 2, 2 ) = at( 1, 0 ) =                           avg2( east( 5 ), east( 6 ) );
  at( 3, 3 ) = at( 2, 1 ) =                           avg3( east( 5 ), east( 6 ), east( 7 ) );
  at( 3, 2 ) = at( 2, 0 ) =                           avg2( east( 6 ), east( 7 ) );
  at( 3, 1 ) =                                        avg3( east( 6 ), east( 7 ), east( 8 ) );
  at( 3, 0 ) =                                        avg2( east( 7 ), east( 8 ) );
}

template <>
void Raster::Block4::vertical_left_predict( void )
{
  at( 0, 0 ) =                                        avg2( above( 0 ), above( 1 ) );
  at( 0, 1 ) =                                        avg3( above( 0 ), above( 1 ), above( 2 ) );
  at( 0, 2 ) = at( 1, 0 ) =                           avg2( above( 1 ), above( 2 ) );
  at( 1, 1 ) = at( 0, 3 ) =                           avg3( above( 1 ), above( 2 ), above( 3 ) );
  at( 1, 2 ) = at( 2, 0 ) =                           avg2( above( 2 ), above( 3 ) );
  at( 1, 3 ) = at( 2, 1 ) =                           avg3( above( 2 ), above( 3 ), above( 4 ) );
  at( 2, 2 ) = at( 3, 0 ) =                           avg2( above( 3 ), above( 4 ) );
  at( 2, 3 ) = at( 3, 1 ) =                           avg3( above( 3 ), above( 4 ), above( 5 ) );
  at( 3, 2 ) =                                        avg3( above( 4 ), above( 5 ), above( 6 ) );
  at( 3, 3 ) =                                        avg3( above( 5 ), above( 6 ), above( 7 ) );
}

template <>
void Raster::Block4::horizontal_down_predict( void )
{
  at( 0, 3 ) =                                        avg2( east( 0 ), east( 1 ) );
  at( 1, 3 ) =                                        avg3( east( 0 ), east( 1 ), east( 2 ) );
  at( 0, 2 ) = at( 2, 3 ) =                           avg2( east( 1 ), east( 2 ) );
  at( 1, 2 ) = at( 3, 3 ) =                           avg3( east( 1 ), east( 2 ), east( 3 ) );
  at( 2, 2 ) = at( 0, 1 ) =                           avg2( east( 2 ), east( 3 ) );
  at( 3, 2 ) = at( 1, 1 ) =                           avg3( east( 2 ), east( 3 ), east( 4 ) );
  at( 2, 1 ) = at( 0, 0 ) =                           avg2( east( 3 ), east( 4 ) );
  at( 3, 1 ) = at( 1, 0 ) =                           avg3( east( 3 ), east( 4 ), east( 5 ) );
  at( 2, 0 ) =                                        avg3( east( 4 ), east( 5 ), east( 6 ) );
  at( 3, 0 ) =                                        avg3( east( 5 ), east( 6 ), east( 7 ) );
}

template <>
void Raster::Block4::horizontal_up_predict( void )
{
  at( 0, 0 ) =                                        avg2( left( 0 ), left( 1 ) );
  at( 1, 0 ) =                                        avg3( left( 0 ), left( 1 ), left( 2 ) );
  at( 2, 0 ) = at( 0, 1 ) =                           avg2( left( 1 ), left( 2 ) );
  at( 3, 0 ) = at( 1, 1 ) =                           avg3( left( 1 ), left( 2 ), left( 3 ) );
  at( 2, 1 ) = at( 0, 2 ) =                           avg2( left( 2 ), left( 3 ) );
  at( 3, 1 ) = at( 1, 2 ) =                           avg3( left( 2 ), left( 3 ), left( 3 ) );
  at( 2, 2 ) = at( 3, 2 ) = at( 0, 3 )
    = at( 1, 3 ) = at( 2, 3 ) = at( 3, 3 ) =          left( 3 );
}

template <>
template <>
void Raster::Block4::intra_predict( const bmode b_mode )
{
  /* Luma prediction */

  switch ( b_mode ) {
  case B_DC_PRED: dc_predict_simple(); break; 
  case B_TM_PRED: true_motion_predict(); break;
  case B_VE_PRED: vertical_smoothed_predict(); break;
  case B_HE_PRED: horizontal_smoothed_predict(); break;
  case B_LD_PRED: left_down_predict(); break;
  case B_RD_PRED: right_down_predict(); break;
  case B_VR_PRED: vertical_right_predict(); break;
  case B_VL_PRED: vertical_left_predict(); break;
  case B_HD_PRED: horizontal_down_predict(); break;
  case B_HU_PRED: horizontal_up_predict(); break;
  default: assert( false ); break;
  }
}

class EdgeExtendedRaster
{
private:
  const TwoD< uint8_t > & master_;

public:
  EdgeExtendedRaster( const TwoD< uint8_t > & master )
    : master_( master ) {}

  uint8_t at( const int column, const int row ) const
  {
    int bounded_column = column;
    if ( bounded_column < 0 ) bounded_column = 0;
    if ( bounded_column > int(master_.width() - 1) ) bounded_column = master_.width() - 1;

    int bounded_row = row;
    if ( bounded_row < 0 ) bounded_row = 0;
    if ( bounded_row > int(master_.height() - 1) ) bounded_row = master_.height() - 1;

    return master_.at( bounded_column, bounded_row );
  }
};

static const SafeArray< SafeArray< int16_t, 6 >, 8 > sixtap_filters =
  {{ { 0,  0,  128,    0,   0,  0 },
     { 0, -6,  123,   12,  -1,  0 },
     { 2, -11, 108,   36,  -8,  1 },
     { 0, -9,   93,   50,  -6,  0 },
     { 3, -16,  77,   77, -16,  3 },
     { 0, -6,   50,   93,  -9,  0 },
     { 1, -8,   36,  108, -11,  2 },
     { 0, -1,   12,  123,  -6,  0 } }};

template <unsigned int size>
void Raster::Block<size>::safe_inter_predict( const MotionVector & mv, const TwoD< uint8_t > & reference )
{
  const int source_column = context().column * size + (mv.x() >> 3);
  const int source_row = context().row * size + (mv.y() >> 3);

  if ( source_column - 2 < 0
       or source_column + size + 3 > reference.width()
       or source_row - 2 < 0
       or source_row + size + 3 > reference.height() ) {

    EdgeExtendedRaster safe_reference( reference );

    inter_predict( mv, safe_reference, source_column, source_row );
  } else {
    unsafe_inter_predict( mv, reference, source_column, source_row );
  }
}

template <unsigned int size>
void Raster::Block<size>::unsafe_inter_predict( const MotionVector & mv, const TwoD< uint8_t > & reference,
						const int source_column,
						const int source_row )
{
  assert( contents_.stride() == reference.width() );

  const unsigned int stride = contents_.stride();

  const uint8_t mx = mv.x() & 7, my = mv.y() & 7;

  if ( (mx & 7) == 0 and (my & 7) == 0 ) {
    uint8_t *dest_row_start = &contents_.at( 0, 0 );
    const uint8_t *src_row_start = &reference.at( source_column, source_row );
    const uint8_t *dest_last_row_start = dest_row_start + size * contents_.stride();
    while ( dest_row_start != dest_last_row_start ) {
      memcpy( dest_row_start, src_row_start, size );
      dest_row_start += stride;
      src_row_start += stride;
    }
    return;
  }

  SafeArray< SafeArray< uint8_t, size >, size + 5 > intermediate;

  {
    uint8_t *intermediate_row_start = &intermediate.at( 0 ).at( 0 );
    const uint8_t *intermediate_last_row_start = intermediate_row_start + size * (size + 5);
    const uint8_t *src_row_start = &reference.at( source_column - 2, source_row - 2 );

    const auto & horizontal_filter = sixtap_filters.at( mx );

    while ( intermediate_row_start != intermediate_last_row_start ) {
      const uint8_t *intermediate_row_end = intermediate_row_start + size;

      while ( intermediate_row_start != intermediate_row_end ) {
	*( intermediate_row_start ) =
	  clamp255( ( (   *( src_row_start )     * horizontal_filter.at( 0 ) )
		      + ( *( src_row_start + 1 ) * horizontal_filter.at( 1 ) )
		      + ( *( src_row_start + 2 ) * horizontal_filter.at( 2 ) )
		      + ( *( src_row_start + 3 ) * horizontal_filter.at( 3 ) )
		      + ( *( src_row_start + 4 ) * horizontal_filter.at( 4 ) )
		      + ( *( src_row_start + 5 ) * horizontal_filter.at( 5 ) )
		      + 64 ) >> 7 );

	intermediate_row_start++;
	src_row_start++;
      }
      src_row_start += stride - size;
    }
  }

  {
    uint8_t *dest_row_start = &contents_.at( 0, 0 );
    const uint8_t *dest_last_row_start = dest_row_start + size * stride;
    const uint8_t *intermediate_row_start = &intermediate.at( 0 ).at( 0 );

    const auto & vertical_filter = sixtap_filters.at( my );

    while ( dest_row_start != dest_last_row_start ) {
      const uint8_t *dest_row_end = dest_row_start + size;

      while ( dest_row_start != dest_row_end ) {
	*dest_row_start =
	  clamp255( ( (   *( intermediate_row_start )            * vertical_filter.at( 0 ) )
		      + ( *( intermediate_row_start + size )     * vertical_filter.at( 1 ) )
		      + ( *( intermediate_row_start + size * 2 ) * vertical_filter.at( 2 ) )
		      + ( *( intermediate_row_start + size * 3 ) * vertical_filter.at( 3 ) )
		      + ( *( intermediate_row_start + size * 4 ) * vertical_filter.at( 4 ) )
		      + ( *( intermediate_row_start + size * 5 ) * vertical_filter.at( 5 ) )
		      + 64 ) >> 7 );

	dest_row_start++;
	intermediate_row_start++;
      }
      dest_row_start += stride - size;
    }
  }
}

template <unsigned int size>
template <class ReferenceType>
void Raster::Block<size>::inter_predict( const MotionVector & mv, const ReferenceType & reference,
					 const int source_column, const int source_row )
{
  if ( (mv.x() & 7) == 0 and (mv.y() & 7) == 0 ) {
    contents_.forall_ij( [&] ( uint8_t & val, unsigned int column, unsigned int row )
			 { val = reference.at( source_column + column,
					       source_row + row ); } );
    return;
  }

  /* filter horizontally */
  const auto & horizontal_filter = sixtap_filters.at( mv.x() & 7 );

  SafeArray< SafeArray< uint8_t, size >, size + 5 > intermediate;

  for ( uint8_t row = 0; row < size + 5; row++ ) {
    for ( uint8_t column = 0; column < size; column++ ) {
      const int real_row = source_row + row - 2;
      const int real_column = source_column + column;
      intermediate.at( row ).at( column ) =
	clamp255( ( ( reference.at( real_column - 2,   real_row ) * horizontal_filter.at( 0 ) )
		    + ( reference.at( real_column - 1, real_row ) * horizontal_filter.at( 1 ) )
		    + ( reference.at( real_column,     real_row ) * horizontal_filter.at( 2 ) )
		    + ( reference.at( real_column + 1, real_row ) * horizontal_filter.at( 3 ) )
		    + ( reference.at( real_column + 2, real_row ) * horizontal_filter.at( 4 ) )
		    + ( reference.at( real_column + 3, real_row ) * horizontal_filter.at( 5 ) )
		    + 64 ) >> 7 );
    }
  }

  /* filter vertically */
  const auto & vertical_filter = sixtap_filters.at( mv.y() & 7 );

  for ( uint8_t row = 0; row < size; row++ ) {
    for ( uint8_t column = 0; column < size; column++ ) {
      contents_.at( column, row ) =
	clamp255( ( ( intermediate.at( row ).at( column ) * vertical_filter.at( 0 ) )
		    + ( intermediate.at( row + 1 ).at( column ) * vertical_filter.at( 1 ) )
		    + ( intermediate.at( row + 2 ).at( column ) * vertical_filter.at( 2 ) )
		    + ( intermediate.at( row + 3 ).at( column ) * vertical_filter.at( 3 ) )
		    + ( intermediate.at( row + 4 ).at( column ) * vertical_filter.at( 4 ) )
		    + ( intermediate.at( row + 5 ).at( column ) * vertical_filter.at( 5 ) )
		    + 64 ) >> 7 );
    }
  }
}
