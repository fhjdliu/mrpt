/* +---------------------------------------------------------------------------+
   |                     Mobile Robot Programming Toolkit (MRPT)               |
   |                          http://www.mrpt.org/                             |
   |                                                                           |
   | Copyright (c) 2005-2016, Individual contributors, see AUTHORS file        |
   | See: http://www.mrpt.org/Authors - All rights reserved.                   |
   | Released under BSD License. See details in http://www.mrpt.org/License    |
   +---------------------------------------------------------------------------+ */

#include "maps-precomp.h" // Precomp header

#include <mrpt/maps/CRandomFieldGridMap3D.h>
#include <mrpt/utils/CConfigFileBase.h>
#include <mrpt/utils/CTicTac.h>
#include <mrpt/utils/CFileOutputStream.h>

using namespace mrpt;
using namespace mrpt::maps;
using namespace mrpt::utils;
using namespace std;

IMPLEMENTS_SERIALIZABLE(CRandomFieldGridMap3D, CSerializable,mrpt::maps)

/*---------------------------------------------------------------
						Constructor
  ---------------------------------------------------------------*/
CRandomFieldGridMap3D::CRandomFieldGridMap3D(
	double x_min, double x_max,
	double y_min, double y_max,
	double z_min, double z_max,
	double voxel_size,
	bool call_initialize_now
) :
	CDynamicGrid3D<TRandomFieldVoxel>( x_min,x_max,y_min,y_max, z_min, z_max, voxel_size /*xy*/, voxel_size /*z*/ ),
	COutputLogger("CRandomFieldGridMap3D")
{
	if (call_initialize_now)
		this->internal_initialize();
}

/** Changes the size of the grid, erasing previous contents */
void CRandomFieldGridMap3D::setSize(
	const double x_min, const double x_max,
	const double y_min, const double y_max,
	const double z_min, const double z_max,
	const double resolution_xy, const double resolution_z,
	const  TRandomFieldVoxel* fill_value)
{
	MRPT_START;

	CDynamicGrid3D<TRandomFieldVoxel>::setSize(x_min, x_max, y_min, y_max, z_min, z_max, resolution_xy, resolution_z, fill_value);
	this->internal_initialize();

	MRPT_END;
}

void  CRandomFieldGridMap3D::resize(
	double new_x_min, double new_x_max,
	double new_y_min, double new_y_max,
	double new_z_min, double new_z_max,
	const TRandomFieldVoxel& defaultValueNewCells, double additionalMarginMeters)
{
	MRPT_START;

	CDynamicGrid3D<TRandomFieldVoxel>::resize(new_x_min, new_x_max, new_y_min, new_y_max, new_z_min, new_z_max, defaultValueNewCells, additionalMarginMeters);
	this->internal_initialize(false);

	MRPT_END;
}

void CRandomFieldGridMap3D::clear()
{
	internal_initialize();
}

void CRandomFieldGridMap3D::internal_initialize(bool erase_prev_contents)
{
	if (erase_prev_contents)
	{
		// Set the gridmap (m_map) to initial values:
		TRandomFieldVoxel  def(0, 0); // mean, std
		fill(def);
	}

	// Reset gmrf:
	m_gmrf.setVerbosityLevel(this->getMinLoggingLevel());
	if (erase_prev_contents)
	{
		m_gmrf.clear();
		m_mrf_factors_activeObs.clear();
	}
	else 
	{
		// Only remove priors, leave observations:
		m_gmrf.clearAllConstraintsByType_Binary();
	}
	m_mrf_factors_priors.clear();

	// Initiating prior (fully connected)
	const size_t nodeCount = m_map.size();
	ASSERT_EQUAL_(nodeCount, m_size_x*m_size_y*m_size_z);
	ASSERT_EQUAL_(m_size_x_times_y, m_size_x*m_size_y);

	MRPT_LOG_DEBUG_STREAM << "[internal_initialize] Creating priors for GMRF with " << nodeCount << " nodes." << std::endl;
	CTicTac tictac;
	tictac.Tic();

	m_mrf_factors_activeObs.resize(nodeCount); // Alloc space for obs
	m_gmrf.initialize(nodeCount);

	size_t cx = 0, cy = 0, cz = 0;
	for (size_t j = 0; j<nodeCount; j++)
	{
		// add factors between this node and: 
		// 1) the right node: j +1
		// 2) the back node:  j+m_size_x
		// 3) the top node: j+m_size_x*m_size*y
		//-------------------------------------------------
		const size_t c_idx_to_check[3] = { cx,cy,cz };
		const size_t c_idx_to_check_limits[3] = { m_size_x - 1,m_size_y - 1,m_size_z - 1 };
		const size_t c_neighbor_idx_incr[3] = { 1,m_size_x,m_size_x_times_y };

		for (int dir = 0; dir < 3; dir++)
		{
			if (c_idx_to_check[dir] >= c_idx_to_check_limits[dir])
				continue;

			const size_t i = j + c_neighbor_idx_incr[dir];
			ASSERT_(i<nodeCount);

			TPriorFactorGMRF new_prior(*this);
			new_prior.node_id_i = i;
			new_prior.node_id_j = j;
			new_prior.Lambda = insertionOptions.GMRF_lambdaPrior;

			m_mrf_factors_priors.push_back(new_prior);
			m_gmrf.addConstraint(*m_mrf_factors_priors.rbegin()); // add to graph
		}

		// Increment coordinates:
		if (++cx >= m_size_x) {
			cx = 0;
			if (++cy >= m_size_y) {
				cy = 0;
				cz++;
			}
		}
	} // end for "j"

	MRPT_LOG_DEBUG_STREAM << "[internal_initialize] Prior built in " << tictac.Tac() << " s\n" << std::endl;
}

/*---------------------------------------------------------------
					TInsertionOptions
 ---------------------------------------------------------------*/
CRandomFieldGridMap3D::TInsertionOptions::TInsertionOptions() :
	GMRF_lambdaPrior			( 0.01f ),		// [GMRF model] The information (Lambda) of fixed map constraints
	GMRF_skip_variance			(false)
{
}

void  CRandomFieldGridMap3D::TInsertionOptions::dumpToTextStream(mrpt::utils::CStream	&out) const
{
	out.printf("GMRF_lambdaPrior                     = %f\n", GMRF_lambdaPrior);
	out.printf("GMRF_skip_variance                   = %s\n", GMRF_skip_variance ? "true":"false");
}

void  CRandomFieldGridMap3D::TInsertionOptions::loadFromConfigFile(
	const mrpt::utils::CConfigFileBase  &iniFile,
	const std::string &section)
{
	GMRF_lambdaPrior = iniFile.read_double(section.c_str(), "GMRF_lambdaPrior", GMRF_lambdaPrior);
	GMRF_skip_variance = iniFile.read_bool(section.c_str(),"GMRF_skip_variance", GMRF_skip_variance);
}


bool mrpt::maps::CRandomFieldGridMap3D::saveAsCSV(const std::string & filName_mean, const std::string & filName_stddev) const
{
	CFileOutputStream f_mean, f_stddev;

	if (!f_mean.open(filName_mean))
		return false;

	if (!filName_stddev.empty()) {
		if (!f_stddev.open(filName_stddev))
			return false;
	}

	const size_t nodeCount = m_map.size();
	size_t cx = 0, cy = 0, cz = 0;
	for (size_t j = 0; j<nodeCount; j++)
	{
		const double x = idx2x(cx), y = idx2y(cy), z = idx2z(cz);
		const double mean_val = m_map[j].mean_value;
		const double stddev_val = m_map[j].stddev_value;

		f_mean.printf("%f %f %f %e\n", x, y, z, mean_val);
		
		if (f_stddev.is_open())
			f_stddev.printf("%f %f %f %e\n", x, y, z, stddev_val);

		// Increment coordinates:
		if (++cx >= m_size_x) {
			cx = 0;
			if (++cy >= m_size_y) {
				cy = 0;
				cz++;
			}
		}
	} // end for "j"

	return true;
}

void CRandomFieldGridMap3D::updateMapEstimation()
{
	ASSERTMSG_(!m_mrf_factors_activeObs.empty(), "Cannot update a map with no observations!");

	Eigen::VectorXd x_incr, x_var;
	m_gmrf.updateEstimation(x_incr, insertionOptions.GMRF_skip_variance ? NULL:&x_var);

	ASSERT_(m_map.size() == x_incr.size());
	ASSERT_(insertionOptions.GMRF_skip_variance || m_map.size() == x_var.size());

	// Update Mean-Variance in the base grid class
	for (size_t j = 0; j<m_map.size(); j++)
	{
		m_map[j].mean_value += x_incr[j];
		m_map[j].stddev_value = insertionOptions.GMRF_skip_variance ? .0 : std::sqrt(x_var[j]);
	}
}

bool CRandomFieldGridMap3D::insertIndividualReading(
	const double sensorReading,              //!< [in] The value observed in the (x,y,z) position
	const double sensorVariance,             //!< [in] The variance of the sensor observation
	const mrpt::math::TPoint3D & point,      //!< [in] The (x,y,z) location
	const TVoxelInterpolationMethod method,  //!< [in] Voxel interpolation method: how many voxels will be affected by the reading
	const bool update_map                    //!< [in] Run a global map update after inserting this observatin (algorithm-dependant)
)
{
	MRPT_START;

	ASSERT_ABOVE_(sensorVariance, .0);
	ASSERTMSG_(m_mrf_factors_activeObs.size()==m_map.size(), "Trying to insert observation in uninitialized map (!)");

	const size_t cell_idx = cellAbsIndexFromCXCYCZ(x2idx(point.x), y2idx(point.y), z2idx(point.z));
	if (cell_idx == INVALID_VOXEL_IDX)
		return false;

	TObservationGMRF new_obs(*this);
	new_obs.node_id = cell_idx;
	new_obs.obsValue = sensorReading;
	new_obs.Lambda = 1.0 / sensorVariance;

	m_mrf_factors_activeObs[cell_idx].push_back(new_obs);
	m_gmrf.addConstraint(*m_mrf_factors_activeObs[cell_idx].rbegin()); // add to graph

	if (update_map)
		this->updateMapEstimation();

	return true;

	MRPT_END;
}

void CRandomFieldGridMap3D::writeToStream(mrpt::utils::CStream &out, int *version) const
{
	if (version)
		*version = 0;
	else
	{
		dyngridcommon_writeToStream(out);

		// To assure compatibility: The size of each cell:
		uint32_t n = static_cast<uint32_t>(sizeof(TRandomFieldVoxel));
		out << n;

		// Save the map contents:
		n = static_cast<uint32_t>(m_map.size());
		out << n;

		// Save the "m_map": This requires special handling for big endian systems:
#if MRPT_IS_BIG_ENDIAN
		for (uint32_t i = 0; i<n; i++)
		{
			out << m_map[i].mean_value << m_map[i].stddev_value;
		}
#else
		// Little endian: just write all at once:
		out.WriteBuffer(&m_map[0], sizeof(m_map[0])*m_map.size());
#endif

		out << insertionOptions.GMRF_lambdaPrior
			<< insertionOptions.GMRF_skip_variance;
	}
}

void CRandomFieldGridMap3D::readFromStream(mrpt::utils::CStream &in, int version)
{
	switch (version)
	{
	case 0:
	{
		dyngridcommon_readFromStream(in);

		// To assure compatibility: The size of each cell:
		uint32_t n;
		in >> n;

		ASSERT_EQUAL_(n, static_cast<uint32_t>(sizeof(TRandomFieldVoxel)));
		// Load the map contents:
		in >> n;
		m_map.resize(n);

		// Read the note in writeToStream()
#if MRPT_IS_BIG_ENDIAN
		for (uint32_t i = 0; i<n; i++)
			in >> m_map[i].mean_value >> m_map[i].stddev_value;
#else
		// Little endian: just read all at once:
		in.ReadBuffer(&m_map[0], sizeof(m_map[0])*m_map.size());
#endif
		in >> insertionOptions.GMRF_lambdaPrior
		   >> insertionOptions.GMRF_skip_variance;

	} break;
	default:
		MRPT_THROW_UNKNOWN_SERIALIZATION_VERSION(version);
	};

}


// ============ TObservationGMRF ===========
double CRandomFieldGridMap3D::TObservationGMRF::evaluateResidual() const
{
	return m_parent->m_map[this->node_id].mean_value - this->obsValue;
}
double CRandomFieldGridMap3D::TObservationGMRF::getInformation() const
{
	return this->Lambda;
}
void CRandomFieldGridMap3D::TObservationGMRF::evalJacobian(double &dr_dx) const
{
	dr_dx = 1.0;
}
// ============ TPriorFactorGMRF ===========
double CRandomFieldGridMap3D::TPriorFactorGMRF::evaluateResidual() const
{
	return m_parent->m_map[this->node_id_i].mean_value - m_parent->m_map[this->node_id_j].mean_value;
}
double CRandomFieldGridMap3D::TPriorFactorGMRF::getInformation() const
{
	return this->Lambda;
}
void CRandomFieldGridMap3D::TPriorFactorGMRF::evalJacobian(double &dr_dx_i, double &dr_dx_j) const
{
	dr_dx_i = +1.0;
	dr_dx_j = -1.0;
}

